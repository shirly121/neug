/** Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hnsw_index.h"

#include <glog/logging.h>
#include <filesystem>
#include <stdexcept>

#include "neug/storages/checkpoint.h"
#include "zvec_dump_container.h"

namespace neug::extension::zvec_ext {

using namespace zvec::core_interface;

static MetricType ParseMetricType(const std::string& metric_str) {
  if (metric_str == "ip" || metric_str == "inner_product") {
    return MetricType::kInnerProduct;
  } else if (metric_str == "cosine") {
    return MetricType::kCosine;
  }
  return MetricType::kL2sq;  // default
}

HNSWIndex::HNSWIndex(std::string name, std::unique_ptr<neug::IndexMeta> meta)
    : Index(std::move(name), std::move(meta)) {
  // Extract HNSW params from meta_->options.
  // zvec_index_ and doc_id_map_ are created later in Open().
  auto it = meta_->options.find("dimension");
  if (it != meta_->options.end())
    dimension_ = std::stoi(it->second);
  it = meta_->options.find("m");
  if (it != meta_->options.end())
    m_ = std::stoi(it->second);
  it = meta_->options.find("ef_construction");
  if (it != meta_->options.end())
    ef_construction_ = std::stoi(it->second);
  it = meta_->options.find("metric");
  if (it != meta_->options.end())
    metric_ = ParseMetricType(it->second);
}

void HNSWIndex::Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
                     MemoryLevel level) {
  // 1. Call base to restore IndexMeta and DocIDMap
  Index::Open(ckp, descriptor, level);

  // 2. Extract HNSW params from meta_->options
  auto it = meta_->options.find("dimension");
  if (it != meta_->options.end())
    dimension_ = std::stoi(it->second);
  it = meta_->options.find("m");
  if (it != meta_->options.end())
    m_ = std::stoi(it->second);
  it = meta_->options.find("ef_construction");
  if (it != meta_->options.end())
    ef_construction_ = std::stoi(it->second);
  it = meta_->options.find("metric");
  if (it != meta_->options.end())
    metric_ = ParseMetricType(it->second);

  // 3. Create runtime path
  std::string runtime_uuid = ckp.CreateRuntimeObject();
  zvec_runtime_path_ = ckp.runtime_dir() + "/" + runtime_uuid;

  // 4. Get index_buffer path from descriptor
  auto index_path = descriptor.get_path("index_buffer");
  bool has_existing = index_path.has_value() && !index_path->empty() &&
                      std::filesystem::exists(*index_path);

  // 5. Copy existing or prepare fresh
  if (has_existing && *index_path != zvec_runtime_path_) {
    std::filesystem::copy(*index_path, zvec_runtime_path_,
                          std::filesystem::copy_options::overwrite_existing |
                              std::filesystem::copy_options::recursive);
  } else if (!has_existing) {
    std::filesystem::remove_all(zvec_runtime_path_);
  }

  // 6. Create and open zvec index
  HNSWIndexParam param(metric_, dimension_, m_, ef_construction_);
  param.data_type = zvec::core_interface::DataType::DT_FP32;
  zvec_index_ = IndexFactory::CreateAndInitIndex(param);
  if (!zvec_index_) {
    throw std::runtime_error(
        "[zvec] Failed to create HNSW index: IndexFactory returned null");
  }

  StorageOptions opts;
  opts.type = StorageOptions::StorageType::kMMAP;
  opts.create_new = !has_existing;
  opts.read_only = false;

  int ret = zvec_index_->Open(zvec_runtime_path_, opts);
  if (ret != 0) {
    throw std::runtime_error(
        "[zvec] Failed to open HNSW index at: " + zvec_runtime_path_ +
        ", error code: " + std::to_string(ret));
  }
  LOG(INFO) << "[zvec] Opened HNSW index at runtime path: "
            << zvec_runtime_path_;
}

ModuleDescriptor HNSWIndex::Dump(Checkpoint& ckp) {
  // Call base to persist IndexMeta and DocIDMap
  auto desc = Index::Dump(ckp);
  desc.module_type = "hnsw_index";

  // Persist HNSW index via ZVecDumpContainer
  if (zvec_index_) {
    ZVecDumpContainer container(zvec_index_.get(), zvec_runtime_path_);
    desc.set_path("index_buffer", ckp.Commit(container));
  }

  return desc;
}

Status HNSWIndex::AppendImpl(vid_t vid, doc_id_t doc_id,
                             const std::vector<Property>& values) {
  // Extract vector data from values[0] (string_view holding raw float bytes)
  if (values.empty()) {
    return Status::RuntimeError("[zvec] AppendImpl: no values provided");
  }
  auto sv = values[0].as_string_view();
  int dim = static_cast<int>(sv.size() / sizeof(float));
  if (dim != dimension_) {
    return Status::RuntimeError("[zvec] Dimension mismatch: expected " +
                                std::to_string(dimension_) + ", got " +
                                std::to_string(dim));
  }

  VectorData vd{DenseVector{sv.data()}};
  int ret = zvec_index_->Add(vd, doc_id);
  if (ret != 0) {
    return Status::RuntimeError("[zvec] Add failed, error code: " +
                                std::to_string(ret));
  }
  return Status::OK();
}

Status HNSWIndex::Search(const IndexQueryParams& params,
                         const IndexFilterParams& filter_params,
                         std::vector<vid_t>& results) {
  auto& hnsw_params = dynamic_cast<const neug::HNSWIndexQueryParams&>(params);

  if (static_cast<int>(hnsw_params.target_vec.size()) != dimension_) {
    return Status::RuntimeError("[zvec] Search dimension mismatch: expected " +
                                std::to_string(dimension_) + ", got " +
                                std::to_string(hnsw_params.target_vec.size()));
  }

  // Build query param
  auto search_param = std::make_shared<HNSWQueryParam>();
  search_param->topk = hnsw_params.topK;
  search_param->ef_search =
      std::max(static_cast<uint32_t>(hnsw_params.topK), kDefaultHnswEfSearch);

  // TODO: Build MVCC filter from IndexFilterParams when filter support is added

  VectorData qd{DenseVector{hnsw_params.target_vec.data()}};
  SearchResult result;
  int ret = zvec_index_->Search(qd, search_param, &result);
  if (ret != 0) {
    return Status::RuntimeError("[zvec] Search failed, error code: " +
                                std::to_string(ret));
  }

  // Convert doc_id -> vid via DocIDMap
  results.reserve(result.doc_list_.size());
  for (const auto& doc : result.doc_list_) {
    vid_t vid = doc_id_map_->GetVID(static_cast<doc_id_t>(doc.key()));
    if (vid != INVALID_VID) {
      results.push_back(vid);
    }
  }
  return Status::OK();
}

Status HNSWIndex::Delete(vid_t vid) {
  doc_id_map_->Erase(vid);
  return Status::OK();
}

std::shared_ptr<Index> HNSWIndex::Fork() const {
  auto forked = std::make_shared<HNSWIndex>();
  forked->meta_ = std::make_unique<IndexMeta>(*meta_);
  if (doc_id_map_) {
    forked->doc_id_map_ = doc_id_map_->Clone();
  }
  forked->zvec_index_ = zvec_index_;  // shared_ptr copy, refcount+1
  forked->dimension_ = dimension_;
  forked->m_ = m_;
  forked->ef_construction_ = ef_construction_;
  forked->metric_ = metric_;
  return forked;
}

void HNSWIndex::LazyFork() {
  // With unique_ptr<DocIDMap>, Fork already deep-copies.
  // No additional lazy fork needed.
}

}  // namespace neug::extension::zvec_ext
