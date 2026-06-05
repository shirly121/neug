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

#include "zvec/core/interface/index_param.h"

namespace neug::extension::zvec_ext {

/**
 * @brief Register HNSW index creator in the NeuG IndexFactory.
 *
 * Factory creates HNSWIndex via default constructor. The transaction
 * is set later via SetStorageInterface(). Open() restores state from
 * the ModuleDescriptor.
 */
static void RegisterHNSWIndex() {
  auto creator =
      [](const neug::ModuleDescriptor& desc) -> std::unique_ptr<neug::Index> {
    // Create via default constructor; Open() will restore full state
    auto index = std::make_unique<HNSWIndex>();

    // Pre-populate meta_ from descriptor for factory consumers that
    // read meta before calling Open()
    neug::IndexMeta meta;
    meta.type = "HNSW";
    auto name_str = desc.get("name");
    if (name_str.has_value()) {
      meta.name = name_str.value();
    }
    auto dim_str = desc.get("dimension");
    if (dim_str.has_value()) {
      meta.options["dimension"] = dim_str.value();
    }
    auto m_str = desc.get("m");
    if (m_str.has_value()) {
      meta.options["m"] = m_str.value();
    } else {
      meta.options["m"] =
          std::to_string(zvec::core_interface::kDefaultHnswNeighborCnt);
    }
    auto ef_str = desc.get("ef_construction");
    if (ef_str.has_value()) {
      meta.options["ef_construction"] = ef_str.value();
    } else {
      meta.options["ef_construction"] =
          std::to_string(zvec::core_interface::kDefaultHnswEfConstruction);
    }
    auto metric_str = desc.get("metric");
    if (metric_str.has_value()) {
      meta.options["metric"] = metric_str.value();
    } else {
      meta.options["metric"] = "l2";
    }

    return index;
  };

  neug::IndexFactory::Instance().RegisterCreator("hnsw_index", creator);
  neug::IndexFactory::Instance().RegisterCreator("HNSW", creator);

  // Register with ModuleFactory so ModuleBroker can restore from checkpoint
  neug::ModuleFactory::instance().Register("hnsw_index", [] {
    return std::make_unique<neug::extension::zvec_ext::HNSWIndex>();
  });

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
