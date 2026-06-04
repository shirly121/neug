#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "neug/compiler/planner/operator/logical_operator.h"

namespace neug {
namespace planner {

struct CreateIndexInfo {
  std::string indexName;
  std::string tableName;  // vertex label
  std::string indexType;  // "HNSW", "IVF"
  std::vector<std::string> propertyNames;
  std::unordered_map<std::string, std::string> options;  // WITH clause
  bool ifNotExists = false;
};

class LogicalCreateIndex final : public LogicalOperator {
  static constexpr LogicalOperatorType type_ =
      LogicalOperatorType::CREATE_INDEX;

 public:
  explicit LogicalCreateIndex(CreateIndexInfo info)
      : LogicalOperator{type_}, info{std::move(info)} {}

  void computeFactorizedSchema() override;
  void computeFlatSchema() override;
  std::string getExpressionsForPrinting() const override;

  const CreateIndexInfo& getInfo() const { return info; }

  std::unique_ptr<LogicalOperator> copy() override {
    return std::make_unique<LogicalCreateIndex>(info);
  }

 private:
  CreateIndexInfo info;
};

}  // namespace planner
}  // namespace neug
