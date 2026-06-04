#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "neug/compiler/parser/statement.h"

namespace neug {
namespace parser {

struct ParsedCreateIndexInfo {
  std::string indexName;
  std::string tableName;  // vertex label (ON clause)
  std::string indexType;  // USING clause: "HNSW", "IVF", etc.
  std::vector<std::string> propertyNames;
  std::unordered_map<std::string, std::string> options;  // WITH clause
  bool ifNotExists = false;
};

class CreateIndex final : public Statement {
 public:
  explicit CreateIndex(ParsedCreateIndexInfo info)
      : Statement{common::StatementType::CREATE_INDEX}, info{std::move(info)} {}

  const ParsedCreateIndexInfo& getInfo() const { return info; }

 private:
  ParsedCreateIndexInfo info;
};

}  // namespace parser
}  // namespace neug
