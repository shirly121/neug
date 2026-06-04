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

#include <string>

#include "neug/storages/container/i_container.h"
#include "zvec/core/interface/index.h"

namespace neug::extension::zvec_ext {

/**
 * @brief Adapter that wraps a ZVec index as an IDataContainer.
 *
 * This allows the ZVec index data to be committed through the Checkpoint
 * framework's Commit() mechanism. The container does not own the zvec index;
 * it only provides Sync/Dump/GetPath so Checkpoint can manage the file.
 */
class ZVecDumpContainer : public IDataContainer {
 public:
  ZVecDumpContainer(zvec::core_interface::Index* index,
                    std::string runtime_path)
      : zvec_index_(index), runtime_path_(std::move(runtime_path)) {}

  ~ZVecDumpContainer() override = default;

  ContainerType GetContainerType() const override {
    return ContainerType::kFileSharedMMap;
  }

  void Resize(size_t /*size*/) override {
    // No-op: zvec manages its own storage size.
  }

  std::string GetPath() const override { return runtime_path_; }

  void Open(const std::string& /*path*/) override {
    // No-op: zvec index is already opened externally.
  }

  void Sync() override {
    if (zvec_index_) {
      zvec_index_->Flush();
    }
  }

  void Dump(const std::string& new_path) override {
    if (zvec_index_) {
      zvec_index_->Flush();
    }
    // After Flush, the data is already at runtime_path_.
    // If new_path != runtime_path_, the checkpoint framework handles
    // the file copy/link via CommitToSnapshot.
  }

  bool IsDirty() override {
    // Always report dirty so checkpoint commits it.
    return true;
  }

 private:
  zvec::core_interface::Index* zvec_index_;
  std::string runtime_path_;
};

}  // namespace neug::extension::zvec_ext
