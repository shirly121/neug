#include "sqlite_fts_extension.h"

#include "neug/compiler/extension/extension_api.h"
#include "sqlite_fts_function.h"
#include "sqlite_fts_index_scan.h"

extern "C" {

void Init() {
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
          neug::sqlite_fts_ext::kExtensionCatalogName,
          "Provides SQLite FTS indexes and full-text search."});
}

const char* Name() { return neug::sqlite_fts_ext::kExtensionName; }

}  // extern "C"
