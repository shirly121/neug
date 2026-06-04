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

#include "neug/storages/index/index_manager.h"

#include <glog/logging.h>

#include "neug/storages/index/index_factory.h"

namespace neug {

static constexpr const char* kIndexPrefix = "index_";
static constexpr const char* kDocIdMapSuffix = "/doc_id_map";

neug::result<Index*> IndexManager::CreateIndex(const std::string& name,
                                               const IndexMeta& meta) {
  if (indexes_.count(name) > 0) {
    RETURN_STATUS_ERROR(StatusCode::ERR_SCHEMA_MISMATCH,
                        "Index already exists: " + name);
  }

  // Create index via factory using a default descriptor
  ModuleDescriptor desc;
  desc.module_type = meta.type;
  auto index = IndexFactory::Instance().Create(meta.type, desc);
  if (!index) {
    RETURN_STATUS_ERROR(StatusCode::ERR_NOT_FOUND,
                        "Unknown index type: " + meta.type);
  }

  auto* raw_ptr = index.get();
  indexes_[name] = std::shared_ptr<Index>(index.release());
  return raw_ptr;
}

Status IndexManager::DropIndex(const std::string& name) {
  auto it = indexes_.find(name);
  if (it == indexes_.end()) {
    return Status::RuntimeError("Index not found: " + name);
  }
  indexes_.erase(it);
  return Status::OK();
}

Status IndexManager::GetIndex(const std::string& label_name,
                              const std::vector<std::string>& property_names,
                              std::vector<Index*>& target_indexes) {
  for (auto& [name, index] : indexes_) {
    if (!index)
      continue;
    const auto& meta = index->GetMeta();
    if (meta.schema.label.label_name == label_name) {
      // Check if properties match
      bool match = true;
      for (const auto& prop : property_names) {
        bool found = false;
        for (const auto& idx_prop : meta.schema.property_names) {
          if (idx_prop == prop) {
            found = true;
            break;
          }
        }
        if (!found) {
          match = false;
          break;
        }
      }
      if (match) {
        target_indexes.push_back(index.get());
      }
    }
  }
  return Status::OK();
}

Status IndexManager::GetAllIndexes(std::vector<Index*>& target_indexes) {
  for (auto& [name, index] : indexes_) {
    if (index) {
      target_indexes.push_back(index.get());
    }
  }
  return Status::OK();
}

void IndexManager::Open(Checkpoint& ckp, const CheckpointManifest& meta,
                        MemoryLevel level) {
  for (const auto& [key, desc] : meta.modules()) {
    // Match keys with "index_" prefix but not "/doc_id_map" suffix
    if (key.substr(0, strlen(kIndexPrefix)) != kIndexPrefix) {
      continue;
    }
    if (key.find(kDocIdMapSuffix) != std::string::npos) {
      continue;
    }

    auto index = IndexFactory::Instance().Create(desc.module_type, desc);
    if (!index) {
      LOG(WARNING) << "Unknown index type in manifest: " << desc.module_type
                   << " (key=" << key << ")";
      continue;
    }

    // Look for the corresponding doc_id_map descriptor
    std::string doc_id_map_key = key + kDocIdMapSuffix;
    auto doc_id_map_desc = meta.module(doc_id_map_key);

    // Open the index - it will handle its own doc_id_map internally
    index->Open(ckp, desc, level);

    // If there's a separate doc_id_map descriptor, open it
    if (doc_id_map_desc.has_value()) {
      // The doc_id_map is managed by the Index itself via its protected member.
      // We pass the descriptor info through the index's own descriptor.
      // The Index::Open implementation is responsible for opening its DocIDMap.
    }

    indexes_[key] = std::shared_ptr<Index>(index.release());
    LOG(INFO) << "Opened index: " << key << " (type=" << desc.module_type
              << ")";
  }
}

void IndexManager::Dump(Checkpoint& ckp, CheckpointManifest& meta) {
  for (auto& [name, index] : indexes_) {
    if (!index)
      continue;

    auto desc = index->Dump(ckp);
    meta.set_module(name, std::move(desc));
  }
}

std::shared_ptr<IndexManager> IndexManager::Fork() const {
  auto forked = std::make_shared<IndexManager>();
  for (const auto& [name, index] : indexes_) {
    if (index) {
      forked->indexes_[name] = index->Fork();
    }
  }
  return forked;
}

}  // namespace neug
