#pragma once

#include "neug/compiler/binder/ddl/bound_create_index.h"
#include "neug/compiler/planner/operator/logical_operator.h"

namespace neug {
namespace planner {

class LogicalCreateIndex final : public LogicalOperator {
  static constexpr LogicalOperatorType type_ =
      LogicalOperatorType::CREATE_INDEX;

 public:
  explicit LogicalCreateIndex(binder::BoundCreateIndexInfo info)
      : LogicalOperator{type_}, info{std::move(info)} {}

  void computeFactorizedSchema() override;
  void computeFlatSchema() override;
  std::string getExpressionsForPrinting() const override;

  const binder::BoundCreateIndexInfo& getInfo() const { return info; }

  std::unique_ptr<LogicalOperator> copy() override {
    return std::make_unique<LogicalCreateIndex>(info.copy());
  }

 private:
  binder::BoundCreateIndexInfo info;
};

}  // namespace planner
}  // namespace neug
