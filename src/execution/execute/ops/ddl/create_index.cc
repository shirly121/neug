/** Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "neug/execution/execute/ops/ddl/create_index.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "neug/utils/pb_utils.h"

namespace neug {
namespace execution {
namespace ops {

class CreateIndexOpr : public IOperator {
 public:
  CreateIndexOpr(std::string index_name, std::string vertex_type,
                 std::string index_type, std::vector<std::string> properties,
                 std::unordered_map<std::string, std::string> options,
                 bool ignore_conflict)
      : index_name_(std::move(index_name)),
        vertex_type_(std::move(vertex_type)),
        index_type_(std::move(index_type)),
        properties_(std::move(properties)),
        options_(std::move(options)),
        ignore_conflict_(ignore_conflict) {}

  std::string get_operator_name() const override { return "CreateIndexOpr"; }

  neug::result<Context> Eval(IStorageInterface& graph, const ParamsMap& params,
                             Context&& ctx, OprTimer* timer) override {
    // TODO: Implement actual index creation via storage interface
    // For now, this is a placeholder that will be connected to the
    // vector index storage layer in a future phase.
    return neug::result<Context>(std::move(ctx));
  }

 private:
  std::string index_name_;
  std::string vertex_type_;
  std::string index_type_;
  std::vector<std::string> properties_;
  std::unordered_map<std::string, std::string> options_;
  bool ignore_conflict_;
};

neug::result<OpBuildResultT> CreateIndexOprBuilder::Build(
    const Schema& schema, const ContextMeta& ctx_meta,
    const physical::PhysicalPlan& plan, int op_id) {
  ContextMeta meta = ctx_meta;
  const auto& create_index = plan.plan(op_id).opr().create_index();

  std::string index_name = create_index.name();
  std::string vertex_type = create_index.vertex_type().name();
  std::string index_type = create_index.index_type();

  std::vector<std::string> properties;
  for (const auto& prop : create_index.properties()) {
    properties.push_back(prop);
  }

  std::unordered_map<std::string, std::string> options;
  for (const auto& [key, value] : create_index.options()) {
    options[key] = value;
  }

  bool ignore_conflict =
      !conflict_action_to_bool(create_index.conflict_action());

  return std::make_pair(
      std::make_unique<CreateIndexOpr>(
          std::move(index_name), std::move(vertex_type), std::move(index_type),
          std::move(properties), std::move(options), ignore_conflict),
      meta);
}

}  // namespace ops
}  // namespace execution
}  // namespace neug
