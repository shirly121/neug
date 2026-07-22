#include "hybrid_search_extension.h"

#include <glog/logging.h>

#include "hnsw_index.h"
#include "hnsw_index_scan.h"
#include "neug/compiler/extension/extension_api.h"
#include "neug/storages/module/module_factory.h"
#include "sqlite_fts_function.h"
#include "sqlite_fts_index_scan.h"
#include "vector_distance_function.h"

extern "C" {

void RegisterModules() {
  neug::ModuleFactory::instance().Register<neug::zvec_ext::HNSWIndex>();
}

void Init() {
  RegisterModules();

  neug::extension::ExtensionAPI::registerFunction<
      neug::zvec_ext::VectorDistanceL2Function>(
      neug::catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
  neug::extension::ExtensionAPI::registerFunction<
      neug::zvec_ext::VectorDistanceCosineFunction>(
      neug::catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
  neug::extension::ExtensionAPI::registerFunction<
      neug::zvec_ext::VectorDistanceIPFunction>(
      neug::catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
  neug::extension::ExtensionAPI::registerFunction<
      neug::zvec_ext::HNSWIndexScanFunction>(
      neug::catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY);
  neug::extension::ExtensionAPI::registerRule<
      neug::zvec_ext::HNSWIndexScanOptimizer>(
      neug::catalog::CatalogEntryType::RULE_ENTRY);

  neug::extension::ExtensionAPI::registerFunction<
      neug::sqlite_fts_ext::SQLiteFTSBM25Function>(
      neug::catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
  neug::extension::ExtensionAPI::registerFunction<
      neug::sqlite_fts_ext::SQLiteFTSIndexScanFunction>(
      neug::catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY);
  neug::extension::ExtensionAPI::registerRule<
      neug::sqlite_fts_ext::SQLiteFTSIndexScanOptimizer>(
      neug::catalog::CatalogEntryType::RULE_ENTRY);

  neug::extension::ExtensionAPI::registerExtension(
      neug::extension::ExtensionInfo{
          neug::hybrid_search_ext::kExtensionCatalogName,
          "Provides vector and full-text indexes for hybrid search."});
  LOG(INFO) << "[hybrid_search extension] initialized";
}

const char* Name() { return neug::hybrid_search_ext::kExtensionName; }

}  // extern "C"
