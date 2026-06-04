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

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "neug/storages/index/i_index.h"
#include "neug/storages/module_descriptor.h"

namespace neug {

/**
 * @brief Factory for creating Index instances by type name.
 *
 * Extensions register their index implementations (e.g. "hnsw_index") via
 * RegisterCreator(). The storage layer calls Create() to obtain an Index
 * without depending on a specific extension at compile time.
 *
 * Thread-safe: all operations are guarded by an internal mutex.
 */
class IndexFactory {
 public:
  /// A creator function that builds an Index from a ModuleDescriptor.
  using CreatorFunc =
      std::function<std::unique_ptr<Index>(const ModuleDescriptor& desc)>;

  static IndexFactory& Instance() {
    static IndexFactory instance;
    return instance;
  }

  void RegisterCreator(const std::string& type_name, CreatorFunc creator) {
    std::lock_guard<std::mutex> lock(mutex_);
    creators_[type_name] = std::move(creator);
  }

  std::unique_ptr<Index> Create(const std::string& type_name,
                                const ModuleDescriptor& desc) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = creators_.find(type_name);
    if (it == creators_.end()) {
      return nullptr;
    }
    return it->second(desc);
  }

  bool HasType(const std::string& type_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return creators_.find(type_name) != creators_.end();
  }

  std::vector<std::string> GetRegisteredTypes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> types;
    types.reserve(creators_.size());
    for (const auto& [name, _] : creators_) {
      types.push_back(name);
    }
    return types;
  }

 private:
  IndexFactory() = default;
  IndexFactory(const IndexFactory&) = delete;
  IndexFactory& operator=(const IndexFactory&) = delete;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, CreatorFunc> creators_;
};

}  // namespace neug
