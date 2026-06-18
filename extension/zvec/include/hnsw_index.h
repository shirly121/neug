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

#pragma once

#include <memory>
#include <string>

#include "neug/storages/index/i_index.h"
#include "zvec/core/interface/index.h"
#include "zvec/core/interface/index_factory.h"
#include "zvec/core/interface/index_param.h"

namespace neug::extension::zvec_ext {

using MetricType = zvec::core_interface::MetricType;

/**
 * @brief HNSW index implementation backed by ZVec.
 *
 * Directly inherits neug::Index (which inherits Module).
 * Internally holds a zvec::core_interface::Index for vector operations.
 * Supports COW: zvec_index_ is shared across forks, only DocIDMap is forked.
 */
class HNSWIndex : public neug::Index {
 public:
  static std::string type_name() { return "hnsw_index"; }

  HNSWIndex() = default;
  HNSWIndex(std::string name, std::unique_ptr<neug::IndexMeta> meta);
  ~HNSWIndex() override = default;

  // Module interface
  void Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
            MemoryLevel level) override;
  ModuleDescriptor Dump(Checkpoint& ckp) override;
  std::string ModuleTypeName() const override { return "hnsw_index"; }

  // Index interface
  Status Search(const IndexQueryParams& params,
                const IndexFilterParams& filter_params,
                std::vector<vid_t>& results) override;
  Status Delete(vid_t vid) override;
  std::unique_ptr<Index> Fork() const override;
  void LazyFork() override;

 protected:
  Status AppendImpl(vid_t vid, doc_id_t doc_id,
                    const std::vector<Property>& values) override;

 private:
  /// ZVec raw index -- shared across forks (COW: only DocIDMap is forked)
  std::shared_ptr<zvec::core_interface::Index> zvec_index_;
  /// Runtime path for zvec storage (temp dir for constructor, snapshot for
  /// Open)
  std::string zvec_runtime_path_;
  /// HNSW params (extracted from meta_.options)
  int dimension_ = 0;
  int m_ = 50;
  int ef_construction_ = 500;
  MetricType metric_ = MetricType::kL2sq;
};

}  // namespace neug::extension::zvec_ext
