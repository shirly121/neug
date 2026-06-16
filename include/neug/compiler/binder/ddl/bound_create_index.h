#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "neug/compiler/binder/bound_statement.h"
#include "neug/compiler/binder/expression/node_rel_expression.h"
#include "neug/compiler/common/types/types.h"
#include "neug/compiler/function/table/table_function.h"

namespace neug {
namespace binder {

struct BoundCreateIndexInfo {
  std::string indexName;
  // Catalog-validated pattern (NodeOrRelExpression holds TableCatalogEntry*)
  std::shared_ptr<NodeOrRelExpression> pattern;
  std::vector<std::string> propertyNames;
  // Property types resolved from catalog
  std::vector<common::LogicalType> propertyTypes;
  // CREATE_<TYPE>_INDEX function looked up from catalog
  function::TableFunction indexCreateFunc;
  std::unordered_map<std::string, std::string> options;
  bool ifNotExists = false;

  BoundCreateIndexInfo copy() const {
    BoundCreateIndexInfo result;
    result.indexName = indexName;
    result.pattern = pattern;
    result.propertyNames = propertyNames;
    result.propertyTypes = common::LogicalType::copy(propertyTypes);
    result.indexCreateFunc = indexCreateFunc;
    result.options = options;
    result.ifNotExists = ifNotExists;
    return result;
  }
};

class BoundCreateIndex final : public BoundStatement {
 public:
  explicit BoundCreateIndex(BoundCreateIndexInfo info)
      : BoundStatement{common::StatementType::CREATE_INDEX,
                       BoundStatementResult::createSingleStringColumnResult()},
        info{std::move(info)} {}

  const BoundCreateIndexInfo& getInfo() const { return info; }
  BoundCreateIndexInfo moveInfo() { return std::move(info); }

 private:
  BoundCreateIndexInfo info;
};

}  // namespace binder
}  // namespace neug
