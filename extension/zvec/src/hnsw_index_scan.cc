#include "hnsw_index_scan.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "hnsw_index.h"
#include "neug/common/columns/vertex_columns.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/catalog/catalog_entry/function_catalog_entry.h"
#include "neug/compiler/common/value_converter.h"
#include "neug/compiler/function/built_in_function_utils.h"
#include "neug/compiler/function/table/bind_data.h"
#include "neug/compiler/planner/operator/logical_order_by.h"
#include "neug/compiler/planner/operator/logical_projection.h"
#include "neug/compiler/planner/operator/logical_table_function_call.h"
#include "neug/compiler/planner/operator/scan/logical_scan_node_table.h"
#include "neug/execution/common/context.h"
#include "neug/generated/proto/plan/expr.pb.h"
#include "neug/storages/graph/graph_interface.h"
#include "neug/storages/index/storage_index_manager.h"
#include "neug/utils/exception/exception.h"

namespace neug::zvec_ext {
namespace {

bool IsVectorDistanceFunction(const function::ScalarFunction& function) {
  return function.name == "VECTOR_DISTANCE_L2" ||
         function.name == "VECTOR_DISTANCE_COSINE" ||
         function.name == "VECTOR_DISTANCE_IP";
}

const binder::ScalarFunctionExpression* FindDistanceExpression(
    const planner::LogicalOrderBy& order_by,
    const planner::LogicalProjection& projection) {
  const auto order_expressions = order_by.getExpressionsToOrderBy();
  if (order_expressions.size() != 1) {
    return nullptr;
  }

  const auto& order_expression = order_expressions[0];
  const auto matches_order = [&](const function::ScalarFunction& function) {
    if (!IsVectorDistanceFunction(function)) {
      return false;
    }
    const bool is_inner_product = function.name == "VECTOR_DISTANCE_IP";
    return order_by.getIsAscOrders()[0] != is_inner_product;
  };
  if (order_expression->expressionType == common::ExpressionType::FUNCTION) {
    auto function =
        order_expression->ptrCast<binder::ScalarFunctionExpression>();
    if (matches_order(function->getFunction())) {
      return function;
    }
  }

  for (const auto& expression : projection.getExpressionsToProject()) {
    if (expression->expressionType != common::ExpressionType::FUNCTION ||
        expression->getUniqueName() != order_expression->getUniqueName()) {
      continue;
    }
    auto function = expression->ptrCast<binder::ScalarFunctionExpression>();
    if (matches_order(function->getFunction())) {
      return function;
    }
  }
  return nullptr;
}

bool ExtractDistanceArguments(const binder::ScalarFunctionExpression& distance,
                              const binder::PropertyExpression*& property,
                              std::shared_ptr<binder::Expression>& target) {
  const auto children = distance.getChildren();
  if (children.size() != 2) {
    return false;
  }
  for (const auto& child : children) {
    if (child->expressionType == common::ExpressionType::PROPERTY) {
      property = child->ptrCast<binder::PropertyExpression>();
    } else {
      target = child;
    }
  }
  return property != nullptr && target != nullptr;
}

bool ContainsPrimaryKeyPredicate(
    const std::shared_ptr<binder::Expression>& expression,
    common::table_id_t table_id) {
  if (!expression) {
    return false;
  }
  if (expression->expressionType == common::ExpressionType::PROPERTY) {
    return expression->constCast<binder::PropertyExpression>().isPrimaryKey(
        table_id);
  }
  for (const auto& child : expression->getChildren()) {
    if (ContainsPrimaryKeyPredicate(child, table_id)) {
      return true;
    }
  }
  return false;
}

std::shared_ptr<binder::Expression> MakeScanColumn(
    const planner::LogicalScanNodeTable& scan) {
  auto node_id = scan.getNodeID();
  const auto& property = node_id->constCast<binder::PropertyExpression>();
  auto output = std::shared_ptr<binder::Expression>(node_id->copy());
  output->setUniqueName(property.getVariableName());
  output->setAlias(property.getRawVariableName());
  return output;
}

Value ParseScalarValue(const ::common::Value& value) {
  if (value.has_f32()) {
    return Value::FLOAT(value.f32());
  }
  if (value.has_f64()) {
    return Value::DOUBLE(value.f64());
  }
  THROW_RUNTIME_ERROR("HNSW_INDEX_SCAN target must contain numeric values");
}

Value ParseTargetValue(const ::common::Expression& expression) {
  if (expression.operators_size() != 1 ||
      !expression.operators(0).has_to_array()) {
    THROW_RUNTIME_ERROR("HNSW_INDEX_SCAN target must be an array literal");
  }
  const auto& array = expression.operators(0).to_array();
  std::vector<Value> children;
  children.reserve(array.fields_size());
  DataType child_type;
  for (const auto& field : array.fields()) {
    if (field.operators_size() != 1 || !field.operators(0).has_const_()) {
      THROW_RUNTIME_ERROR(
          "HNSW_INDEX_SCAN target array must contain only literals");
    }
    auto value = ParseScalarValue(field.operators(0).const_());
    if (children.empty()) {
      child_type = value.type().copy();
    } else if (value.type() != child_type) {
      THROW_RUNTIME_ERROR(
          "HNSW_INDEX_SCAN target array elements must have one type");
    }
    children.push_back(std::move(value));
  }
  if (children.empty()) {
    THROW_RUNTIME_ERROR("HNSW_INDEX_SCAN target array cannot be empty");
  }
  return Value::ARRAY(DataType::Array(child_type, children.size()),
                      std::move(children));
}

std::unique_ptr<function::CallFuncInputBase> BindHNSWIndexScan(
    const Schema&, const execution::ContextMeta&,
    const physical::PhysicalPlan& plan, int op_idx) {
  const auto& op = plan.plan(op_idx);
  const auto& scan = op.opr().index_scan();
  auto input = std::make_unique<HNSWIndexScanFuncInput>();
  std::string label;
  std::string property;
  std::string topk;
  for (const auto& option : scan.options()) {
    if (option.first == "label_id") {
      label = option.second;
    } else if (option.first == "property_name") {
      property = option.second;
    } else if (option.first == "topk") {
      topk = option.second;
    }
  }
  if (label.empty() || property.empty() || topk.empty()) {
    THROW_RUNTIME_ERROR("HNSW_INDEX_SCAN is missing required options");
  }
  input->label_id = static_cast<label_t>(std::stoul(label));
  input->property_name = std::move(property);
  input->topk = static_cast<uint32_t>(std::stoul(topk));
  input->target_value = ParseTargetValue(scan.target_value());
  input->alias = op.meta_data_size() == 0 ? -1 : op.meta_data(0).alias();
  return input;
}

execution::Context ExecuteHNSWIndexScan(
    const function::CallFuncInputBase& base_input, IStorageInterface& graph) {
  const auto& input = dynamic_cast<const HNSWIndexScanFuncInput&>(base_input);
  auto* reader = dynamic_cast<StorageReadInterface*>(&graph);
  if (reader == nullptr) {
    THROW_RUNTIME_ERROR("HNSW_INDEX_SCAN requires a readable graph");
  }

  auto indexes =
      reader->index_manager().GetIndex(input.label_id, input.property_name);
  if (!indexes.has_value()) {
    THROW_RUNTIME_ERROR(indexes.error().ToString());
  }
  HNSWIndex* hnsw_index = nullptr;
  for (auto* index : indexes.value()) {
    hnsw_index = dynamic_cast<HNSWIndex*>(index);
    if (hnsw_index != nullptr) {
      break;
    }
  }
  if (hnsw_index == nullptr) {
    THROW_RUNTIME_ERROR("HNSW index not found for the requested property");
  }

  HNSWIndexQueryParams params;
  params.target_value = input.target_value;
  params.topk = input.topk;
  params.ef_search = std::max<uint32_t>(input.topk, 100);
  for (const auto& context_chunk : input.context.chunks()) {
    if (!context_chunk.exist(input.alias)) {
      continue;
    }
    params.use_scalar_filter = true;
    auto vertices = std::dynamic_pointer_cast<IVertexColumn>(
        context_chunk.get(input.alias));
    if (!vertices) {
      THROW_RUNTIME_ERROR(
          "HNSW_INDEX_SCAN filter input must be a vertex column");
    }
    params.scalar_filter.reserve(params.scalar_filter.size() +
                                 vertices->size());
    foreach_vertex(*vertices, [&](size_t, label_t label, vid_t vid) {
      if (label == input.label_id && vid != INVALID_VID) {
        params.scalar_filter.push_back(vid);
      }
    });
  }
  auto result = hnsw_index->Search(params);
  if (!result.has_value()) {
    THROW_RUNTIME_ERROR(result.error().ToString());
  }

  MSVertexColumnBuilder builder(input.label_id);
  builder.reserve(result->size());
  for (auto vid : result.value()) {
    builder.push_back_opt(vid);
  }
  execution::Context context;
  execution::ContextChunk chunk;
  chunk.set(input.alias, builder.finish());
  context.append_chunk(std::move(chunk));
  return context;
}

}  // namespace

function::function_set HNSWIndexScanFunction::getFunctionSet() {
  auto function = std::make_unique<function::NeugCallFunction>(
      name, function::call_input_types{}, function::call_output_columns{});
  function->bindFunc = BindHNSWIndexScan;
  function->execFunc = ExecuteHNSWIndexScan;
  function::function_set result;
  result.push_back(std::move(function));
  return result;
}

void HNSWIndexScanOptimizer::rewrite(main::ClientContext* context,
                                     planner::LogicalPlan* plan) {
  context_ = context;
  optimizer::LogicalRule::rewrite(context, plan);
  context_ = nullptr;
}

std::shared_ptr<planner::LogicalOperator>
HNSWIndexScanOptimizer::visitOrderByReplace(
    std::shared_ptr<planner::LogicalOperator> op) {
  if (context_ == nullptr) {
    return op;
  }
  auto order_by = op->ptrCast<planner::LogicalOrderBy>();
  if (!order_by->isTopK() ||
      (order_by->getSkipNum() != 0 && order_by->getSkipNum() != UINT64_MAX) ||
      order_by->getLimitNum() == 0 || order_by->getNumChildren() != 1) {
    return op;
  }
  auto child = order_by->getChild(0);
  if (child->getOperatorType() != planner::LogicalOperatorType::PROJECTION) {
    return op;
  }
  auto projection = child->ptrCast<planner::LogicalProjection>();
  if (projection->getNumChildren() != 1 ||
      projection->getChild(0)->getOperatorType() !=
          planner::LogicalOperatorType::SCAN_NODE_TABLE) {
    return op;
  }
  auto scan_op = projection->getChild(0);
  auto scan = scan_op->ptrCast<planner::LogicalScanNodeTable>();
  if (scan->getTableIDs().size() != 1 ||
      scan->getScanType() != planner::LogicalScanNodeTableType::SCAN) {
    return op;
  }
  if (ContainsPrimaryKeyPredicate(scan->getPredicates(),
                                  scan->getTableIDs()[0])) {
    return op;
  }
  const bool has_filter = scan->getPredicates() != nullptr ||
                          !scan->getPropertyPredicates().empty();

  auto distance = FindDistanceExpression(*order_by, *projection);
  if (distance == nullptr) {
    return op;
  }
  const binder::PropertyExpression* property = nullptr;
  std::shared_ptr<binder::Expression> target;
  if (!ExtractDistanceArguments(*distance, property, target) ||
      property->getVariableName() != scan->getAliasName() ||
      property->getSingleTableID() != scan->getTableIDs()[0]) {
    return op;
  }

  auto* function = GetIndexScanFunction(*context_->getCatalog());
  if (function == nullptr) {
    return op;
  }
  binder::expression_vector columns{MakeScanColumn(*scan)};
  auto bind_data =
      std::make_unique<function::IndexScanBindData>(columns, "", target);
  bind_data->options["label_id"] = std::to_string(scan->getTableIDs()[0]);
  bind_data->options["property_name"] = property->getPropertyName();
  bind_data->options["topk"] = std::to_string(order_by->getLimitNum());

  auto table_call = std::make_shared<planner::LogicalTableFunctionCall>(
      *function, std::move(bind_data));
  if (has_filter) {
    table_call->addChild(std::move(scan_op));
  }
  table_call->computeFlatSchema();
  projection->setChild(0, std::move(table_call));
  return op;
}

function::TableFunction* HNSWIndexScanOptimizer::GetIndexScanFunction(
    catalog::Catalog& catalog) const {
  auto* transaction = &transaction::DUMMY_TRANSACTION;
  if (!catalog.containsFunction(transaction, HNSWIndexScanFunction::name)) {
    return nullptr;
  }
  auto* entry =
      catalog.getFunctionEntry(transaction, HNSWIndexScanFunction::name);
  if (entry == nullptr ||
      entry->getType() != catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY) {
    return nullptr;
  }
  auto* function = function::BuiltInFunctionsUtils::matchFunction(
      HNSWIndexScanFunction::name, {},
      entry->ptrCast<catalog::FunctionCatalogEntry>());
  return dynamic_cast<function::TableFunction*>(function);
}

}  // namespace neug::zvec_ext
