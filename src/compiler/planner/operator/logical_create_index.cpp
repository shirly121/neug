#include "neug/compiler/planner/operator/ddl/logical_create_index.h"

namespace neug {
namespace planner {

void LogicalCreateIndex::computeFlatSchema() { createEmptySchema(); }

void LogicalCreateIndex::computeFactorizedSchema() { createEmptySchema(); }

std::string LogicalCreateIndex::getExpressionsForPrinting() const {
  return info.indexName;
}

}  // namespace planner
}  // namespace neug
