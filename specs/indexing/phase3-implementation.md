# Phase 3: Create Index 端到端实现方案

## 概述

Phase 3 完成了 CREATE INDEX 功能的端到端验证，覆盖：
- **3.1** Compiler 单元测试
- **3.2** Engine 层 CreateIndexOpr 执行
- **3.3** Python 端到端测试

## 3.1 Compiler 单元测试

### 测试文件

`tests/compiler/create_index_test.cpp` — 4 个测试用例，验证 CREATE INDEX Cypher → Physical Plan 完整链路。

### 测试用例

| 测试名 | Cypher | 验证重点 |
|--------|--------|----------|
| `CREATE_INDEX_BASIC` | `CREATE INDEX vec_hnsw_index ON User USING HNSW (age);` | 基本语法、conflict_action=ON_CONFLICT_THROW |
| `CREATE_INDEX_WITH_OPTIONS` | `...WITH (metric = 'cosine');` | WITH 选项透传到 physical plan |
| `CREATE_INDEX_IF_NOT_EXISTS` | `CREATE INDEX IF NOT EXISTS ...` | conflict_action=ON_CONFLICT_DO_NOTHING |
| `CREATE_INDEX_MULTI_COLS` | `...USING HNSW (name, age);` | 多列属性 |

### Expected Physical Plan 格式

```json
{
  "plan": [{
    "opr": {
      "create_index": {
        "name": "vec_hnsw_index",
        "vertex_type": {"name": "User"},
        "index_type": "HNSW",
        "properties": ["age"],
        "options": {},
        "conflict_action": "ON_CONFLICT_THROW"
      }
    },
    "meta_data": []
  }]
}
```

Expected 文件位于 `tests/compiler/resources/ddl_test/CREATE_INDEX_*_physical`。

### 运行方式

```bash
TEST_RESOURCE=/path/to/neug/tests/compiler \
  ./build/tests/compiler/gopt_test --gtest_filter='CreateIndexTest.*'
```

## 3.2 Engine CreateIndexOpr

### 实现文件

- `include/neug/execution/execute/ops/ddl/create_index.h` — `CreateIndexOprBuilder`
- `src/execution/execute/ops/ddl/create_index.cc` — `CreateIndexOpr` + `CreateIndexOprBuilder::Build()`

### 当前状态

`CreateIndexOpr::Eval()` 是 **placeholder**，直接返回 `ctx`，不实际操作存储层。将在后续阶段连接到 IndexManager。

### 注册

在 `src/execution/execute/plan_parser.cc` 中注册：
```cpp
register_operator_builder(std::make_unique<ops::CreateIndexOprBuilder>());
```

### Builder 逻辑

从 Physical Plan 的 `CreateIndexOpr` protobuf 消息中解析：
- `name` → index_name
- `vertex_type.name` → vertex_type
- `index_type` → index_type (e.g. "HNSW")
- `properties` → property names
- `options` → key-value map
- `conflict_action` → ignore_conflict bool

## 3.3 Python 端到端测试

### 测试文件

`tools/python_bind/tests/test_hnsw_index.py` — `TestCreateIndex` 类，6 个测试用例。

### 测试用例

| 测试名 | 场景 |
|--------|------|
| `test_create_index_empty_graph` | 空图 + CREATE INDEX |
| `test_create_index_with_options` | WITH 选项 |
| `test_create_index_if_not_exists` | IF NOT EXISTS |
| `test_create_index_after_insert` | 单点插入后建索引 |
| `test_create_index_after_multiple_inserts` | 多点插入后建索引 |
| `test_create_index_multi_columns` | 多列索引 |

### 运行方式

```bash
cd tools/python_bind
python3 -m pytest -sv tests/test_hnsw_index.py::TestCreateIndex
```

### 当前验证范围

由于 Engine 的 `Eval()` 是 placeholder，测试验证的是：
1. CREATE INDEX DDL 语句能通过完整的编译管线（Parser → Binder → Planner → Physical Plan → Engine）
2. 执行不报错
3. 不验证索引是否实际创建（后续阶段实现）

## Compiler Pipeline 完整链路

```
CREATE INDEX ... (Cypher)
    ↓
ANTLR4 Parser (Cypher.g4: nEUG_CreateIndex rule)
    ↓
Transformer (transform_ddl.cpp: transformCreateIndex)
    ↓  ParsedCreateIndexInfo → CreateIndex statement
Binder (bind_ddl.cpp: bindCreateIndex)
    ↓  BoundCreateIndex
Planner (append_simple.cpp: appendCreateIndex)
    ↓  LogicalCreateIndex
GDDLConverter (g_ddl_converter.cpp: convertCreateIndex)
    ↓  CreateIndexOpr protobuf
Engine (create_index.cc: CreateIndexOprBuilder::Build → CreateIndexOpr::Eval)
    ↓  placeholder: return ctx
```

## 涉及的文件清单

### 新增文件

| 文件 | 说明 |
|------|------|
| `include/neug/compiler/parser/ddl/create_index.h` | ParsedCreateIndexInfo + CreateIndex statement |
| `include/neug/compiler/binder/ddl/bound_create_index.h` | BoundCreateIndex |
| `include/neug/compiler/planner/operator/ddl/logical_create_index.h` | LogicalCreateIndex + CreateIndexInfo |
| `src/compiler/planner/operator/logical_create_index.cpp` | computeFlatSchema/computeFactorizedSchema |
| `include/neug/execution/execute/ops/ddl/create_index.h` | CreateIndexOprBuilder |
| `src/execution/execute/ops/ddl/create_index.cc` | CreateIndexOpr + Builder |
| `tests/compiler/create_index_test.cpp` | Compiler 单元测试 |
| `tests/compiler/resources/ddl_test/CREATE_INDEX_*_physical` | Expected physical plan files |

### 修改文件

| 文件 | 修改内容 |
|------|----------|
| `src/compiler/antlr4/Cypher.g4` | nEUG_CreateIndex grammar rule |
| `src/compiler/antlr4/keywords.txt` | INDEX, USING keywords |
| `third_party/antlr4_cypher/cypher_parser.cpp` | 重新生成的 parser |
| `third_party/antlr4_cypher/cypher_lexer.cpp` | 重新生成的 lexer |
| `src/compiler/parser/transformer.cpp` | nEUG_CreateIndex dispatch |
| `src/compiler/parser/transform/transform_ddl.cpp` | transformCreateIndex |
| `src/compiler/parser/parsed_statement_visitor.cpp` | CREATE_INDEX case |
| `include/neug/compiler/parser/parsed_statement_visitor.h` | visitCreateIndex virtual |
| `src/compiler/binder/binder.cpp` | CREATE_INDEX case |
| `src/compiler/binder/bind/bind_ddl.cpp` | bindCreateIndex |
| `include/neug/compiler/binder/binder.h` | bindCreateIndex declaration |
| `src/compiler/binder/bound_statement_visitor.cpp` | CREATE_INDEX case |
| `include/neug/compiler/binder/bound_statement_visitor.h` | visitCreateIndex virtual |
| `src/compiler/planner/planner.cpp` | CREATE_INDEX case |
| `include/neug/compiler/planner/planner.h` | appendCreateIndex declaration |
| `src/compiler/planner/plan/append_simple.cpp` | appendCreateIndex |
| `include/neug/compiler/planner/operator/logical_operator.h` | CREATE_INDEX enum |
| `src/compiler/planner/operator/logical_operator.cpp` | CREATE_INDEX string |
| `src/compiler/planner/operator/CMakeLists.txt` | logical_create_index.cpp |
| `include/neug/compiler/common/enums/statement_type.h` | CREATE_INDEX = 40 |
| `src/compiler/gopt/g_ddl_converter.cpp` | convertCreateIndex |
| `include/neug/compiler/gopt/g_ddl_converter.h` | convertCreateIndex declaration |
| `src/compiler/gopt/g_query_converter.cpp` | CREATE_INDEX dispatch |
| `src/compiler/gopt/g_alias_manager.cpp` | CREATE_INDEX cases |
| `include/neug/compiler/gopt/g_physical_convertor.h` | CREATE_INDEX handling |
| `include/neug/compiler/gopt/g_physical_analyzer.h` | CREATE_INDEX handling |
| `proto/cypher_ddl.proto` | CreateIndexOpr message |
| `proto/physical.proto` | create_index = 75 in oneof |
| `src/execution/execute/plan_parser.cc` | register CreateIndexOprBuilder |
| `tests/compiler/CMakeLists.txt` | add create_index_test.cpp |
| `tools/python_bind/tests/test_hnsw_index.py` | TestCreateIndex class |

## Protobuf 定义

```protobuf
// proto/cypher_ddl.proto
message CreateIndexOpr {
    string name = 1;
    common.NameOrId vertex_type = 2;
    string index_type = 3;
    repeated string properties = 4;
    map<string, string> options = 5;
    ConflictAction conflict_action = 6;
}

// proto/physical.proto — PhysicalOpr.Operator oneof
create_index = 75;
```

## 后续工作

1. **Engine 实际实现**：连接 CreateIndexOpr::Eval() 到 IndexManager，实现真正的索引创建
2. **IndexManager 生命周期**：插入 NeugDB::Open() / Dump() 流程
3. **存量数据建索引**：遍历 VertexTable，批量 Append 到索引
4. **MVCC 集成**：通过 DocIDMap + IndexFilter 支持事务隔离
