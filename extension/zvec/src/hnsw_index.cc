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
#include "neug/storages/graph/graph_interface.h"
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

static std::string MetricTypeToString(MetricType metric) {
  switch (metric) {
  case MetricType::kInnerProduct:
    return "ip";
  case MetricType::kCosine:
    return "cosine";
  default:
    return "l2";
  }
}

HNSWIndex::HNSWIndex(const std::string& name, const neug::IndexMeta& meta,
                     const neug::IStorageInterface& transaction)
    : Index(name, meta, transaction) {
  // Extract HNSW params from meta_.options
  auto it = meta_.options.find("dimension");
  if (it != meta_.options.end())
    dimension_ = std::stoi(it->second);
  it = meta_.options.find("m");
  if (it != meta_.options.end())
    m_ = std::stoi(it->second);
  it = meta_.options.find("ef_construction");
  if (it != meta_.options.end())
    ef_construction_ = std::stoi(it->second);
  it = meta_.options.find("metric");
  if (it != meta_.options.end())
    metric_ = ParseMetricType(it->second);

  // Create the zvec index via factory
  HNSWIndexParam param(metric_, dimension_, m_, ef_construction_);
  param.data_type = zvec::core_interface::DataType::DT_FP32;
  zvec_index_ = IndexFactory::CreateAndInitIndex(param);
  if (!zvec_index_) {
    throw std::runtime_error(
        "[zvec] Failed to create HNSW index: IndexFactory returned null");
  }

  // Open the zvec index with a unique temporary file path so it's ready for
  // Add()
  static std::atomic<uint64_t> counter{0};
  auto tmp = std::filesystem::temp_directory_path() /
             ("neug_zvec_" + std::to_string(getpid()) + "_" +
              std::to_string(counter.fetch_add(1)));
  // Remove stale files from previous runs
  std::filesystem::remove_all(tmp);
  zvec_runtime_path_ = tmp.string();

  StorageOptions opts;
  opts.type = StorageOptions::StorageType::kMMAP;
  opts.create_new = true;
  opts.read_only = false;
  int ret = zvec_index_->Open(zvec_runtime_path_, opts);
  if (ret != 0) {
    throw std::runtime_error(
        "[zvec] Failed to open HNSW index at temp path: " + zvec_runtime_path_ +
        ", error code: " + std::to_string(ret));
  }

  // Create default DocIDMap
  doc_id_map_ = std::make_shared<DocIDMap>();
}

void HNSWIndex::Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
                     MemoryLevel level) {
  // Restore HNSW params from descriptor
  auto dim_str = descriptor.get("dimension");
  if (dim_str.has_value()) {
    dimension_ = std::stoi(dim_str.value());
  }
  auto m_str = descriptor.get("m");
  if (m_str.has_value()) {
    m_ = std::stoi(m_str.value());
  }
  auto ef_str = descriptor.get("ef_construction");
  if (ef_str.has_value()) {
    ef_construction_ = std::stoi(ef_str.value());
  }
  auto metric_str = descriptor.get("metric");
  if (metric_str.has_value()) {
    metric_ = ParseMetricType(metric_str.value());
  }

  // Restore IndexMeta
  auto name_str = descriptor.get("name");
  if (name_str.has_value()) {
    meta_.name = name_str.value();
  }
  meta_.type = "HNSW";
  auto label_str = descriptor.get("label_name");
  if (label_str.has_value()) {
    meta_.schema.label.label_name = label_str.value();
  }

  // Also store params in meta_.options for consistency
  meta_.options["dimension"] = std::to_string(dimension_);
  meta_.options["m"] = std::to_string(m_);
  meta_.options["ef_construction"] = std::to_string(ef_construction_);
  meta_.options["metric"] = MetricTypeToString(metric_);

  // Open DocIDMap from the doc_id_map sub-descriptor
  doc_id_map_ = std::make_shared<DocIDMap>();
  auto doc_id_map_next = descriptor.get("doc_id_map_next_doc_id");
  if (doc_id_map_next.has_value()) {
    ModuleDescriptor doc_desc;
    doc_desc.module_type = "doc_id_map";
    doc_desc.set("next_doc_id", doc_id_map_next.value());
    auto doc_buffer_path = descriptor.get_path("doc_id_map_buffer");
    if (doc_buffer_path.has_value()) {
      doc_desc.set_path("buffer", doc_buffer_path.value());
    }
    doc_id_map_->Open(ckp, doc_desc, level);
  }

  // Open ZVec index from file
  auto zvec_path = descriptor.get_path("zvec_data");
  if (zvec_path.has_value() && !zvec_path->empty()) {
    HNSWIndexParam param(metric_, dimension_, m_, ef_construction_);
    param.data_type = zvec::core_interface::DataType::DT_FP32;
    zvec_index_ = IndexFactory::CreateAndInitIndex(param);

    StorageOptions opts;
    opts.type = StorageOptions::StorageType::kMMAP;
    opts.create_new = false;
    opts.read_only = false;

    int ret = zvec_index_->Open(zvec_path.value(), opts);
    if (ret != 0) {
      throw std::runtime_error(
          "[zvec] Failed to open HNSW index from path: " + zvec_path.value() +
          ", error code: " + std::to_string(ret));
    }
    LOG(INFO) << "[zvec] Opened HNSW index from: " << zvec_path.value();
  } else if (!zvec_index_) {
    // Create a fresh in-memory index
    HNSWIndexParam param(metric_, dimension_, m_, ef_construction_);
    param.data_type = zvec::core_interface::DataType::DT_FP32;
    zvec_index_ = IndexFactory::CreateAndInitIndex(param);
  }
}

ModuleDescriptor HNSWIndex::Dump(Checkpoint& ckp) {
  ModuleDescriptor desc;
  desc.module_type = "hnsw_index";

  // Store HNSW params
  desc.set("dimension", std::to_string(dimension_));
  desc.set("metric", MetricTypeToString(metric_));
  desc.set("m", std::to_string(m_));
  desc.set("ef_construction", std::to_string(ef_construction_));
  desc.set("name", meta_.name);
  desc.set("label_name", meta_.schema.label.label_name);

  // Dump DocIDMap and embed its info
  if (doc_id_map_) {
    auto doc_desc = doc_id_map_->Dump(ckp);
    auto next_doc_id = doc_desc.get("next_doc_id");
    if (next_doc_id.has_value()) {
      desc.set("doc_id_map_next_doc_id", next_doc_id.value());
    }
    auto buffer_path = doc_desc.get_path("buffer");
    if (buffer_path.has_value()) {
      desc.set_path("doc_id_map_buffer", buffer_path.value());
    }
  }

  // Dump ZVec index via runtime object
  if (zvec_index_) {
    std::string runtime_uuid = ckp.CreateRuntimeObject();

    StorageOptions opts;
    opts.type = StorageOptions::StorageType::kMMAP;
    opts.create_new = true;
    opts.read_only = false;

    int ret = zvec_index_->Open(runtime_uuid, opts);
    if (ret != 0) {
      throw std::runtime_error(
          "[zvec] Failed to open index for dump at path: " + runtime_uuid +
          ", error code: " + std::to_string(ret));
    }
    ret = zvec_index_->Flush();
    if (ret != 0) {
      throw std::runtime_error(
          "[zvec] Failed to flush HNSW index, error code: " +
          std::to_string(ret));
    }

    std::string snapshot_path = ckp.CommitRuntimeObject(runtime_uuid);
    desc.set_path("zvec_data", snapshot_path);
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
  auto& hnsw_params = dynamic_cast<const HNSWIndexQueryParams&>(params);

  if (hnsw_params.dimension != dimension_) {
    return Status::RuntimeError("[zvec] Search dimension mismatch: expected " +
                                std::to_string(dimension_) + ", got " +
                                std::to_string(hnsw_params.dimension));
  }

  // Build query param
  auto search_param = std::make_shared<HNSWQueryParam>();
  search_param->topk = hnsw_params.topk;
  search_param->ef_search =
      std::max(static_cast<uint32_t>(hnsw_params.topk), kDefaultHnswEfSearch);

  // Build MVCC filter from DocIDMap + StorageReadInterface::GetVertexSet
  if (transaction_) {
    auto* read_iface =
        dynamic_cast<const neug::StorageReadInterface*>(transaction_);
    if (read_iface) {
      label_t label_id = meta_.schema.label.label_id;
      auto vertex_set = read_iface->GetVertexSet(label_id);

      size_t capacity = vertex_set.size();
      std::vector<bool> valid_vids(capacity, false);
      for (vid_t v : vertex_set) {
        valid_vids[v] = true;
      }

      auto index_filter = std::make_shared<IndexFilter>();
      auto& map = doc_id_map_;
      index_filter->set(
          [map, valid_vids = std::move(valid_vids)](uint64_t key) -> bool {
            doc_id_t doc_id = static_cast<doc_id_t>(key);
            vid_t vid = map->GetVID(doc_id);
            if (vid == INVALID_VID)
              return true;  // exclude
            if (vid >= valid_vids.size())
              return true;
            return !valid_vids[vid];  // exclude if not valid at read_ts
          });
      search_param->filter = index_filter;
    }
  }

  VectorData qd{DenseVector{hnsw_params.query_vector}};
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
  forked->meta_ = meta_;
  forked->doc_id_map_ = doc_id_map_;  // shared_ptr copy, refcount+1
  forked->zvec_index_ = zvec_index_;  // shared_ptr copy, refcount+1
  forked->dimension_ = dimension_;
  forked->m_ = m_;
  forked->ef_construction_ = ef_construction_;
  forked->metric_ = metric_;
  forked->transaction_ = transaction_;
  return forked;
}

void HNSWIndex::LazyFork() {
  if (doc_id_map_.use_count() <= 1) {
    return;
  }
  auto old_map = doc_id_map_;
  doc_id_map_ = old_map;  // keep sharing for reads
  // TODO(shirly): Implement proper lazy fork when Checkpoint is available
  // in the transaction context.
}

}  // namespace neug::extension::zvec_ext
