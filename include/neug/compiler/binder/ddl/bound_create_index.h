#pragma once

#include "neug/compiler/binder/bound_statement.h"
#include "neug/compiler/parser/ddl/create_index.h"

namespace neug {
namespace binder {

class BoundCreateIndex final : public BoundStatement {
 public:
  explicit BoundCreateIndex(parser::ParsedCreateIndexInfo info)
      : BoundStatement{common::StatementType::CREATE_INDEX,
                       BoundStatementResult::createSingleStringColumnResult()},
        info{std::move(info)} {}

  const parser::ParsedCreateIndexInfo& getInfo() const { return info; }

 private:
  parser::ParsedCreateIndexInfo info;
};

}  // namespace binder
}  // namespace neug
