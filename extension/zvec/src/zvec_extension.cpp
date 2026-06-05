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

#include <glog/logging.h>

#include "neug/compiler/extension/extension_api.h"
#include "neug/storages/index/index_factory.h"
#include "neug/storages/module/module_factory.h"
#include "neug/utils/exception/exception.h"

#include "hnsw_index.h"

namespace neug::extension::zvec_ext {

/**
 * @brief Register HNSW index creator in the NeuG IndexFactory.
 *
 * Factory creates HNSWIndex via default constructor.
 * For CreateIndex path: meta is set via Index::SetMeta() after factory
 * creation. For checkpoint restore: Open() restores state from the
 * ModuleDescriptor.
 */
static void RegisterHNSWIndex() {
  neug::IndexFactory::Instance().RegisterCreator(
      "hnsw_index",
      [](const neug::ModuleDescriptor&) -> std::unique_ptr<neug::Index> {
        return std::make_unique<HNSWIndex>();
      });

  neug::ModuleFactory::instance().Register<HNSWIndex>();

  LOG(INFO) << "[zvec extension] Registered HNSW index type";
}

}  // namespace neug::extension::zvec_ext

// Extension entry points (extern "C" for dynamic loading)
extern "C" {

void Init() {
  try {
    neug::extension::zvec_ext::RegisterHNSWIndex();

    neug::extension::ExtensionAPI::registerExtension(
        neug::extension::ExtensionInfo{
            "zvec",
            "Provides vector index implementations (HNSW) backed by ZVec."});

    LOG(INFO) << "[zvec extension] initialized successfully";
  } catch (const std::exception& e) {
    THROW_EXCEPTION_WITH_FILE_LINE("[zvec extension] initialization failed: " +
                                   std::string(e.what()));
  } catch (...) {
    THROW_EXCEPTION_WITH_FILE_LINE(
        "[zvec extension] initialization failed: unknown exception");
  }
}

const char* Name() { return "ZVEC"; }

}  // extern "C"
