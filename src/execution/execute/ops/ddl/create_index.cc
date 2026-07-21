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

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "neug/common/types.h"
#include "neug/storages/graph/graph_interface.h"
#include "neug/storages/index/storage_index.h"
#include "neug/utils/exception/exception.h"
#include "neug/utils/pb_utils.h"
#include "neug/utils/result.h"

namespace neug {
namespace execution {
namespace ops {

class CreateIndexOpr : public IOperator {
 public:
  CreateIndexOpr(std::unique_ptr<IndexMeta> meta, bool ignore_conflict)
      : meta_(std::move(meta)), ignore_conflict_(ignore_conflict) {}

  std::string get_operator_name() const override { return "CreateIndexOpr"; }

  neug::result<Context> Eval(IStorageInterface& graph, const ParamsMap& params,
                             Context&& ctx, OprTimer* timer) override {
    auto* update_interface = dynamic_cast<StorageUpdateInterface*>(&graph);
    if (!update_interface) {
      RETURN_STATUS_ERROR(StatusCode::ERR_NOT_SUPPORTED,
                          "CREATE INDEX can only be executed in update mode");
    }

    const auto& index_name = meta_->name;
    label_t label_id = meta_->schema.label_id;

    auto index_meta = std::make_unique<IndexMeta>(*meta_);
    GS_AUTO(index,
            update_interface->CreateIndex(index_name, std::move(index_meta)));

    const auto property_names = meta_->schema.GetPropertyNames();
    std::vector<std::shared_ptr<RefColumnBase>> columns;
    columns.reserve(property_names.size());
    for (const auto& property_name : property_names) {
      auto column =
          update_interface->GetVertexPropColumn(label_id, property_name);
      if (!column) {
        RETURN_STATUS_ERROR(
            StatusCode::ERR_SCHEMA_MISMATCH,
            "Indexed property column not found: " + property_name);
      }
      columns.push_back(column);
    }
    auto status = index->BeginBuild();
    if (!status.ok()) {
      return tl::unexpected(status);
    }
    auto vertices = update_interface->GetVertexSet(label_id);
    for (vid_t vid : vertices) {
      std::vector<Value> values;
      values.reserve(columns.size());
      for (const auto& column : columns) {
        values.push_back(column->get_any(vid));
      }
      status = index->Upsert(vid, values);
      if (!status.ok()) {
        return tl::unexpected(status);
      }
    }
    status = index->FinishBuild();
    if (!status.ok()) {
      return tl::unexpected(status);
    }

    return std::move(ctx);
  }

 private:
  std::unique_ptr<IndexMeta> meta_;
  bool ignore_conflict_;
};

neug::result<OpBuildResultT> CreateIndexOprBuilder::Build(
    const Schema& schema, const ContextMeta& ctx_meta,
    const physical::PhysicalPlan& plan, int op_id) {
  ContextMeta meta = ctx_meta;
  const auto& create_index = plan.plan(op_id).opr().create_index();

  // Construct IndexMeta from CreateIndex PB:
  //   name -> name
  //   vertex_type -> schema.label
  //   create_index_type -> type
  //   properties -> schema.property_names
  //   property_types -> schema.property_types
  //   options -> options (common::case_insensitive_map_t)

  auto index_meta = std::make_unique<IndexMeta>();
  index_meta->name = create_index.name();

  if (create_index.vertex_type().has_name()) {
    const auto& vtype_name = create_index.vertex_type().name();
    index_meta->schema.label_id = schema.get_vertex_label_id(vtype_name);
  } else {
    index_meta->schema.label_id =
        static_cast<label_t>(create_index.vertex_type().id());
  }

  index_meta->type = create_index.create_index_type();

  for (const auto& prop : create_index.properties()) {
    index_meta->schema.property_names.push_back(prop);
  }

  for (const auto& pt : create_index.property_types()) {
    index_meta->schema.property_types.push_back(parse_from_data_type(pt));
  }
  if (!index_meta->schema.property_names.empty()) {
    index_meta->schema.property_name = index_meta->schema.property_names[0];
  }
  if (!index_meta->schema.property_types.empty()) {
    index_meta->schema.property_type = index_meta->schema.property_types[0];
  }

  for (const auto& [key, value] : create_index.options()) {
    index_meta->options[key] = value;
  }

  bool ignore_conflict =
      !conflict_action_to_bool(create_index.conflict_action());

  return std::make_pair(
      std::make_unique<CreateIndexOpr>(std::move(index_meta), ignore_conflict),
      meta);
}

}  // namespace ops
}  // namespace execution
}  // namespace neug
