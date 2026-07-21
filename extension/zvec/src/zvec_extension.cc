#include <glog/logging.h>

#include "hnsw_index.h"
#include "hnsw_index_scan.h"
#include "neug/compiler/extension/extension_api.h"
#include "neug/storages/module/module_factory.h"
#include "vector_distance_function.h"

extern "C" {

void RegisterModules() {
  // Extension shared-library constructors are not a sufficient registration
  // boundary: LOAD can reuse an already loaded handle, and registration must
  // always target the ModuleFactory owned by the current NeuG host process.
  // Register is idempotent (it replaces the creator for the same type name),
  // so doing this on every registration request is safe.
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
  neug::extension::ExtensionAPI::registerExtension(
      neug::extension::ExtensionInfo{
          "zvec", "Provides ZVec indexes and vector distance functions."});
  LOG(INFO) << "[zvec extension] initialized";
}

const char* Name() { return "ZVEC"; }

}  // extern "C"
