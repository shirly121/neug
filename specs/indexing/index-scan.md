# 支持 IndexScan 

## 语法

```cypher
MATCH (n:VectorNode)
WHERE n.id > 10 
RETURN n.id, n.vec
ORDER BY vec_distance_l2(n.vec, [0.1, 0.1,...]) LIMIT 10
```

对于以上查询，我们生成如下 Physical Plan:

```proto
ProcedureCall {
    query_name = "HNSW_INDEX_SCAN()",
    arguments = [
        params: "{params_json}"
    ]
}
Project(n.id, n.vec)
```

## IndexScanFunction

该函数包含两部分实现：
- call_bind_func_t
- call_exec_func_t

call_bind_func_t 从 pb 中序列化为 HNSWIndexScanFuncInput 结构：

```c++
struct HNSWIndexScanFuncInput : CallFuncInputBase {
    // 从 pb arguments 中反序列化
    static IndexScanFuncInput FromArgs(const procedure::Arguments &args);
    // 序列化为 pb arguments
    procedure::Arguments ToArgs();
    IndexMeta *index_meta;
    common::Value target_vec;
    int topK;
    MetricType metric_type;
}
```

call_exec_func_t 定义如何执行 index scan:
- 从 IndexManager 中获取 Index，by unique_name
- 基于 IndexScanFuncInput 构造 IndexQueryParams
- 调用 Index::Search(IndexQueryParams)


## Optimizer

### 提供规则的可插拔

#### 外部注册

让 zvec extension 可以主动注册 optimizer 规则，需要进一步改进 optimizer 框架。

catalog 中增加规则注册接口：

```c++
class Catalog {
private:
	std::unique_ptr<CatalogSet> rules;
};
```

实现特殊的 CatalogEntry 类型，CatalogEntryType = RULE_ENTRY;

```c++
class RuleCatalogEntry : CatalogEntry {
private:
	std::unique_ptr<LogicalRule> plan_rule;
};

class LogicalRule : LogicalOperatorVisitor {
public:
	virtual void rewrite(planner::LogicalPlan* plan) = 0;
}
```

在 ZVec extension 中实现 HNSWIndexScanOptimizer，继承自 LogicalRule；并通过 catalog 接口注册进来；

#### Compiler Optimizer

Optimizer 层支持执行外部注册的规则；

在 optimizer.cpp 最后，获取当前 Catalog 中的 RuleEntry，foreach 调用 rewrite 函数；

### Rules

提供将上述查询优化为 IndexScanFunction 规则，具体通过下面的 HNSWIndexScanRule 实现。

```c++
class HNSWIndexScanRule : public LogicalRule {
}
```

具体规则实现参考 duckdb 代码：

```c++
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/index.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/table_index_list.hpp"

#include "hnsw/hnsw.hpp"
#include "hnsw/hnsw_index.hpp"
#include "hnsw/hnsw_index_scan.hpp"

namespace duckdb {

//-----------------------------------------------------------------------------
// Plan rewriter
//-----------------------------------------------------------------------------
class HNSWIndexScanOptimizer : public OptimizerExtension {
public:
	HNSWIndexScanOptimizer() {
		optimize_function = Optimize;
	}

	static bool TryOptimize(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
		// Look for a TopN operator
		auto &op = *plan;

		if (op.type != LogicalOperatorType::LOGICAL_TOP_N) {
			return false;
		}

		auto &top_n = op.Cast<LogicalTopN>();

		if (top_n.orders.size() != 1) {
			// We can only optimize if there is a single order by expression right now
			return false;
		}

		const auto &order = top_n.orders[0];

		if (order.type != OrderType::ASCENDING) {
			// We can only optimize if the order by expression is ascending
			return false;
		}

		if (order.expression->type != ExpressionType::BOUND_COLUMN_REF) {
			// The expression has to reference the child operator (a projection with the distance function)
			return false;
		}
		const auto &bound_column_ref = order.expression->Cast<BoundColumnRefExpression>();

		// find the expression that is referenced
		if (top_n.children.size() != 1 || top_n.children.front()->type != LogicalOperatorType::LOGICAL_PROJECTION) {
			// The child has to be a projection
			return false;
		}

		auto &projection = top_n.children.front()->Cast<LogicalProjection>();

		// This the expression that is referenced by the order by expression
		const auto projection_index = bound_column_ref.binding.column_index;
		const auto &projection_expr = projection.expressions[projection_index];

		// The projection must sit on top of a get
		if (projection.children.size() != 1 || projection.children.front()->type != LogicalOperatorType::LOGICAL_GET) {
			return false;
		}

		auto &get_ptr = projection.children.front();
		auto &get = get_ptr->Cast<LogicalGet>();
		// Check if the get is a table scan
		if (get.function.name != "seq_scan") {
			return false;
		}

		if (get.dynamic_filters && get.dynamic_filters->HasFilters()) {
			// Cant push down!
			return false;
		}

		// We have a top-n operator on top of a table scan
		// We can replace the function with a custom index scan (if the table has a custom index)

		// Get the table
		auto &table = *get.GetTable();
		if (!table.IsDuckTable()) {
			// We can only replace the scan if the table is a duck table
			return false;
		}

		auto &duck_table = table.Cast<DuckTableEntry>();
		auto &table_info = *table.GetStorage().GetDataTableInfo();

		// Find the index
		unique_ptr<HNSWIndexScanBindData> bind_data = nullptr;
		vector<reference<Expression>> bindings;

		table_info.BindIndexes(context, HNSWIndex::TYPE_NAME);
		table_info.GetIndexes().Scan([&](Index &index) {
			if (!index.IsBound() || HNSWIndex::TYPE_NAME != index.GetIndexType()) {
				return false;
			}
			auto &cast_index = index.Cast<HNSWIndex>();

			// Reset the bindings
			bindings.clear();

			// Check that the projection expression is a distance function that matches the index
			if (!cast_index.TryMatchDistanceFunction(projection_expr, bindings)) {
				return false;
			}
			// Check that the HNSW index actually indexes the expression
			unique_ptr<Expression> index_expr;
			if (!cast_index.TryBindIndexExpression(get, index_expr)) {
				return false;
			}

			// Now, ensure that one of the bindings is a constant vector, and the other our index expression
			auto &const_expr_ref = bindings[1];
			auto &index_expr_ref = bindings[2];

			if (const_expr_ref.get().type != ExpressionType::VALUE_CONSTANT || !index_expr->Equals(index_expr_ref)) {
				// Swap the bindings and try again
				std::swap(const_expr_ref, index_expr_ref);
				if (const_expr_ref.get().type != ExpressionType::VALUE_CONSTANT ||
				    !index_expr->Equals(index_expr_ref)) {
					// Nope, not a match, we can't optimize.
					return false;
				}
			}

			const auto vector_size = cast_index.GetVectorSize();
			const auto &matched_vector = const_expr_ref.get().Cast<BoundConstantExpression>().value;
			auto query_vector = make_unsafe_uniq_array<float>(vector_size);
			auto vector_elements = ArrayValue::GetChildren(matched_vector);
			for (idx_t i = 0; i < vector_size; i++) {
				query_vector[i] = vector_elements[i].GetValue<float>();
			}

			bind_data = make_uniq<HNSWIndexScanBindData>(duck_table, cast_index, top_n.limit, std::move(query_vector));
			return true;
		});

		if (!bind_data) {
			// No index found
			return false;
		}

		// If there are no table filters pushed down into the get, we can just replace the get with the index scan
		const auto cardinality = get.function.cardinality(context, bind_data.get());
		get.function = HNSWIndexScanFunction::GetFunction();
		get.has_estimated_cardinality = cardinality->has_estimated_cardinality;
		get.estimated_cardinality = cardinality->estimated_cardinality;
		get.bind_data = std::move(bind_data);
		if (get.table_filters.filters.empty()) {

			// Remove the TopN operator
			plan = std::move(top_n.children[0]);
			return true;
		}

		// Otherwise, things get more complicated. We need to pullup the filters from the table scan as our index scan
		// does not support regular filter pushdown.
		get.projection_ids.clear();
		get.types.clear();

		auto new_filter = make_uniq<LogicalFilter>();
		auto &column_ids = get.GetColumnIds();
		for (const auto &entry : get.table_filters.filters) {
			idx_t column_id = entry.first;
			auto &type = get.returned_types[column_id];
			bool found = false;
			for (idx_t i = 0; i < column_ids.size(); i++) {
				if (column_ids[i].GetPrimaryIndex() == column_id) {
					column_id = i;
					found = true;
					break;
				}
			}
			if (!found) {
				throw InternalException("Could not find column id for filter");
			}
			auto column = make_uniq<BoundColumnRefExpression>(type, ColumnBinding(get.table_index, column_id));
			new_filter->expressions.push_back(entry.second->ToExpression(*column));
		}
		new_filter->children.push_back(std::move(get_ptr));
		new_filter->ResolveOperatorTypes();
		get_ptr = std::move(new_filter);

		// Remove the TopN operator
		plan = std::move(top_n.children[0]);
		return true;
	}
};

} // namespace duckdb
```

## ZVec Extension

ZVec 需要注册内容包括：
- HNSWIndexScanRule
- scalar function: vec_distance_l2

