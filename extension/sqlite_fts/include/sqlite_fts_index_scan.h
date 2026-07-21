#pragma once

#include <string>

#include "neug/compiler/function/function.h"
#include "neug/compiler/function/neug_call_function.h"
#include "neug/compiler/optimizer/logical_rule.h"
#include "neug/execution/common/context.h"

namespace neug::sqlite_fts_ext {

struct SQLiteFTSIndexScanFuncInput final : function::CallFuncInputBase {
  label_t label_id;
  std::string property_name;
  std::string query_string;
  uint32_t topk;
  int32_t node_alias;
  int32_t score_alias;
  execution::Context context;

  void bindContext(execution::Context&& input_context) override {
    context = std::move(input_context);
  }
};

struct SQLiteFTSIndexScanFunction {
  static constexpr const char* name = "SQLITE_FTS_INDEX_SCAN";

  static function::function_set getFunctionSet();
};

class SQLiteFTSIndexScanOptimizer final : public optimizer::LogicalRule {
 public:
  static constexpr const char* name = "SQLITE_FTS_INDEX_SCAN_OPTIMIZER";

  void rewrite(main::ClientContext* context,
               planner::LogicalPlan* plan) override;

  std::shared_ptr<planner::LogicalOperator> visitOrderByReplace(
      std::shared_ptr<planner::LogicalOperator> op) override;

 private:
  function::TableFunction* GetIndexScanFunction(
      catalog::Catalog& catalog) const;

  main::ClientContext* context_{nullptr};
};

}  // namespace neug::sqlite_fts_ext
