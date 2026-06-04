/**
 * Copyright 2020 Alibaba Group Holding Limited.
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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "neug/common/types.h"
#include "neug/compiler/common/case_insensitive_map.h"
#include "neug/storages/index/doc_id_map.h"
#include "neug/storages/module/module.h"
#include "neug/utils/property/property.h"
#include "neug/utils/result.h"

namespace neug {

class IStorageInterface;

// --- Index metadata types ---

enum class EntryType : uint8_t { VERTEX = 1, EDGE = 2 };

struct LabelEntry {
  EntryType type = EntryType::VERTEX;
  std::string label_name;
  label_t label_id = 0;
  std::string src_label_name;
  label_t src_label_id = 0;
  std::string dst_label_name;
  label_t dst_label_id = 0;
};

struct IndexBindSchema {
  LabelEntry label;
  std::vector<std::string> property_names;
  std::vector<DataType> property_types;
};

struct IndexMeta {
  std::string name;
  std::string type;
  IndexBindSchema schema;
  common::case_insensitive_map_t<std::string> options;
};

// --- Query/filter parameter types ---

struct IndexQueryParams {
  virtual ~IndexQueryParams() = default;
};

struct IndexFilterParams {};

// --- Index base class ---

/**
 * @brief Abstract base class for all index implementations.
 *
 * Index inherits Module for lifecycle (Open/Dump) and provides the
 * data operations (Search/Append/Delete) plus COW support (Fork/LazyFork).
 *
 * Extensions (e.g. zvec) provide concrete implementations by subclassing
 * Index and registering them through the IndexFactory.
 */
class Index : public Module {
 public:
  Index() = default;

  Index(const std::string& name, const IndexMeta& meta,
        const IStorageInterface& transaction)
      : meta_(meta), transaction_(&transaction) {
    meta_.name = name;
  }

  ~Index() override = default;

  // --- Module interface ---
  std::string ModuleTypeName() const override { return "index"; }

  // --- Data operations ---

  /**
   * @brief Search for nearest neighbors.
   * @param params Query parameters (subclass-specific).
   * @param filter_params MVCC filter (built internally via transaction_).
   * @param results Output: vid_t results after doc_id -> vid translation.
   */
  virtual Status Search(const IndexQueryParams& params,
                        const IndexFilterParams& filter_params,
                        std::vector<vid_t>& results) = 0;

  /**
   * @brief Append a record for the given vertex id.
   *
   * Non-virtual: allocates doc_id via DocIDMap, then delegates to AppendImpl.
   *
   * @param vid The vertex id.
   * @param values Property values for the indexed columns.
   */
  Status Append(vid_t vid, const std::vector<Property>& values) {
    if (!doc_id_map_) {
      return Status::RuntimeError("DocIDMap not initialized");
    }
    doc_id_t doc_id = doc_id_map_->Insert(vid);
    return AppendImpl(vid, doc_id, values);
  }

  /**
   * @brief Mark a vertex as deleted in the index.
   */
  virtual Status Delete(vid_t vid) = 0;

  // --- COW support ---

  /**
   * @brief Shallow copy: shared_ptr shares doc_id_map and raw index.
   */
  virtual std::shared_ptr<Index> Fork() const = 0;

  /**
   * @brief Write-time deep copy of DocIDMap when shared.
   */
  virtual void LazyFork() = 0;

  // --- Metadata ---
  const IndexMeta& GetMeta() const { return meta_; }

  // --- Storage interface ---
  void SetStorageInterface(const IStorageInterface* iface) {
    transaction_ = iface;
  }

 protected:
  /**
   * @brief Subclass-specific append implementation.
   * @param vid The vertex id.
   * @param doc_id The doc_id allocated by DocIDMap.
   * @param values Property values for the indexed columns.
   */
  virtual Status AppendImpl(vid_t vid, doc_id_t doc_id,
                            const std::vector<Property>& values) = 0;

  IndexMeta meta_;
  std::shared_ptr<DocIDMap> doc_id_map_;
  const IStorageInterface* transaction_ = nullptr;
};

}  // namespace neug
