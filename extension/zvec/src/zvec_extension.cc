#include <glog/logging.h>

#include "hnsw_index_scan.h"
#include "neug/compiler/extension/extension_api.h"
#include "vector_distance_function.h"

extern "C" {

void Init() {
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
  neug::extension::ExtensionAPI::registerExtension(
      neug::extension::ExtensionInfo{
          "zvec", "Provides ZVec indexes and vector distance functions."});
  LOG(INFO) << "[zvec extension] initialized";
}

const char* Name() { return "ZVEC"; }

}  // extern "C"
