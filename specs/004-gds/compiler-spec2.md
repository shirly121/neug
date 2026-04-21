# 支持 CALL gds_function

## Cypher API

```cypher
CALL k_core('my_graph', {min_k: 3, concurrency: 4}) YIELD node, core_number;
```

## Compiler 侧

Compiler 侧主要任务就是将上述语法编译成 GDSAlgo 算子，具体定义成 protobuf:

```protobuf
message Subgraph {
  message VertexEntry {
    int32 label_id = 1; // 经过 Schema 绑定后的 label id
    common::Expression predicate = 2; // 经过Schema绑定后的Expression结构，确保子图中的属性在当前版本的schema中存在
  }

  message EdgeEntry {
    int32 src_label_id = 1;
    int32 edge_label_id = 2;
    int32 dst_label_id = 3;
    common::Expression predicate = 4;
  }

  repeated VertexEntry vertex_entries = 1;
  repeated EdgeEntry edge_entries = 2;
}

message GDSAlgo {
    // 算法名称
    string algo_name = 1;
    // 子图信息
    Subgraph sub_graph = 2;
    // 其他配置参数：concurrency, min_k ...
    map<string, string> options = 3;
}
```

在 `include/neug/compiler/function/gds/gds_function.h` 中定义：

```c++
using options_t = common::case_insensitive_map_t<std::string>;

using algo_exec_func_t = std::function<execution::Context(
    execution::Context& ctx, const pb::Subgraph &subgraph,
    const options_t &options,
    const StorageReadInterface& graph)>;

struct GDSFuncBindData : TableFuncBindData {
    // 经过 graph::GDSFunction::bindGraphEntry 得到的 GraphEntry
    graph::GraphEntry;
    // 用户查询中指定 options, {min_k: 3, concurrency: 4}
    options_t options;
};

static std::unique_ptr<GDSFuncBindData> bindGDSFunction(
    main::ClientContext* clientContext, const TableFuncBindInput* input) {
  // 构建 GDSFuncBindData 并返回
}

struct NEUG_API GDSAlgoFunction : public NeuGCallFunction {
  explicit GDSAlgoFunction(std::string name,
                   std::vector<common::LogicalTypeID> inputTypes,
                   call_output_columns outputColumns) : NeugCallFunction{std::move(name), std::move(inputTypes), std::move(outputColumns)} {
    auto *tableFunc = this->ptrCast<TableFunction>(func);
    tableFunc = bindGDSFunction;
  }
  
  // 每个算子需要实现自己的 execFunc 函数
  algo_exec_func_t execFunc;
};
```

将 GDSAlgoFunction 转换成 Physical Plan，具体实现在 `g_query_converter`:

```c++
void GQueryConvertor::convertTableFunc(
    const planner::LogicalTableFunctionCall& funcCall,
    ::physical::PhysicalPlan* plan) {
  auto bindData = funcCall.getBindData();
  if (dynamic_cast<const function::ScanFileBindData*>(bindData)) {
    convertDataSource(funcCall, plan);
  } else if (dynamic_cast<const gds::GDSFuncBindData*>(bindData)) {
    convertGDSFunction(funcCall, plan);
  } else {
    convertProcedureCall(funcCall, plan);
  }
}
```

```c++
// 将 funcCall 转换为 GDSAlgo
// 具体：
// 1. 获取 func_name 转换为 algo_name
// 2. 将 GDSFuncBindData 中的 GraphEntry/options_t 转换为 sub_graph/options
void convertDataSource(
    const planner::LogicalTableFunctionCall& funcCall,
    ::physical::PhysicalPlan* plan) {
}
```