# Refactor CREATE INDEX Compiler Pipeline

## Context

Current `Binder::bindCreateIndex` is a simple passthrough -- it directly passes `ParsedCreateIndexInfo` to the Planner without any catalog validation. Per `specs/indexing/create-index-compiler.md`, the Binder should:

- Validate tableName exists in catalog
- Validate properties exist in the table entry
- Resolve property type info from catalog
- Look up `CREATE_<TYPE>_INDEX` function from catalog (error if not found)

This refactor also updates Protobuf (add `property_types`, rename `index_type` to `create_index_type`), and updates Converter and tests.

**Constraints:**
- Do NOT modify index-related interfaces/implementations (i_index.h, IndexManager, HNSWIndex, etc.)
- Do NOT modify execution layer `CreateIndexOpr` -- comment out if compile errors occur

## Implementation Plan

### Step 1: Protobuf -- `proto/cypher_ddl.proto`

```proto
message CreateIndexOpr {
    string name = 1;
    common.NameOrId vertex_type = 2;
    // Renamed: index_type -> create_index_type
    // Value: function name like "CREATE_HNSW_INDEX"
    string create_index_type = 3;
    repeated string properties = 4;
    // NEW: property type info resolved from catalog
    repeated common.DataType property_types = 5;
    map<string, string> options = 6;       // field number 5 -> 6
    ConflictAction conflict_action = 7;    // field number 6 -> 7
}
```

Note: Field number changes break wire compatibility, but the project rebuilds all components together. `basic_type.proto` is already imported.

### Step 2: Rewrite `BoundCreateIndex` -- `include/neug/compiler/binder/ddl/bound_create_index.h`

Replace `ParsedCreateIndexInfo` with new `BoundCreateIndexInfo`:

```cpp
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "neug/compiler/binder/bound_statement.h"
#include "neug/compiler/binder/expression/node_rel_expression.h"
#include "neug/compiler/common/types/types.h"
#include "neug/compiler/function/table/table_function.h"

namespace neug {
namespace binder {

struct BoundCreateIndexInfo {
  std::string indexName;
  // Catalog-validated pattern (NodeOrRelExpression holds TableCatalogEntry*)
  std::shared_ptr<NodeOrRelExpression> pattern;
  std::vector<std::string> propertyNames;
  // Property types resolved from catalog
  std::vector<common::LogicalType> propertyTypes;
  // CREATE_<TYPE>_INDEX function looked up from catalog
  function::TableFunction indexCreateFunc;
  std::unordered_map<std::string, std::string> options;
  bool ifNotExists = false;

  // For LogicalOperator::copy()
  BoundCreateIndexInfo copy() const;
};

class BoundCreateIndex final : public BoundStatement {
 public:
  explicit BoundCreateIndex(BoundCreateIndexInfo info)
      : BoundStatement{common::StatementType::CREATE_INDEX,
                       BoundStatementResult::createSingleStringColumnResult()},
        info{std::move(info)} {}

  const BoundCreateIndexInfo& getInfo() const { return info; }
  BoundCreateIndexInfo moveInfo() { return std::move(info); }

 private:
  BoundCreateIndexInfo info;
};

}  // namespace binder
}  // namespace neug
```

Key design points:
- `pattern`: `shared_ptr<NodeOrRelExpression>` holds the catalog entry pointer. Table name obtained via `pattern->getSingleEntry()->getName()`.
- `indexCreateFunc`: Actual `function::TableFunction` object looked up from catalog.
- `propertyTypes`: `common::LogicalType` (C++ type). Converter converts to protobuf `DataType`.

### Step 3: Rewrite `Binder::bindCreateIndex` -- `src/compiler/binder/bind/bind_ddl.cpp`

```cpp
std::unique_ptr<BoundStatement> Binder::bindCreateIndex(
    const Statement& statement) {
  auto& createIndex = statement.constCast<CreateIndex>();
  const auto& parsedInfo = createIndex.getInfo();

  // 1. Validate table exists
  validateTableExistence(*clientContext, parsedInfo.tableName);

  // 2. Get table entry, validate it's NODE_TABLE_ENTRY
  auto* tableEntry = clientContext->getCatalog()->getTableCatalogEntry(
      clientContext->getTransaction(), parsedInfo.tableName);
  validateNodeTableType(tableEntry);

  // 3. Create NodeOrRelExpression pattern
  auto pattern = std::make_shared<NodeOrRelExpression>(
      common::LogicalType(common::LogicalTypeID::NODE),
      parsedInfo.tableName, parsedInfo.tableName,
      std::vector<catalog::TableCatalogEntry*>{tableEntry});

  // 4. Validate each property exists, get types
  std::vector<common::LogicalType> propertyTypes;
  for (const auto& propName : parsedInfo.propertyNames) {
    validateColumnExistence(tableEntry, propName);
    const auto& propDef = tableEntry->getProperty(propName);
    propertyTypes.push_back(propDef.getType().copy());
  }

  // 5. Build function name and look up from catalog
  std::string indexTypeUpper = parsedInfo.indexType;
  std::transform(indexTypeUpper.begin(), indexTypeUpper.end(),
                 indexTypeUpper.begin(), ::toupper);
  std::string funcName = "CREATE_" + indexTypeUpper + "_INDEX";

  // Look up function from catalog -- error if not registered
  auto* catalog = clientContext->getCatalog();
  auto* funcEntry = catalog->getFunctionEntry(
      clientContext->getTransaction(), funcName);
  auto* funcCatalogEntry =
      funcEntry->ptrCast<catalog::FunctionCatalogEntry>();
  auto& funcSet = funcCatalogEntry->getFunctionSet();
  // Get the first (and typically only) function from the set
  auto& indexCreateFunc =
      *funcSet[0]->ptrCast<function::TableFunction>();

  // 6. Build BoundCreateIndexInfo
  BoundCreateIndexInfo boundInfo;
  boundInfo.indexName = parsedInfo.indexName;
  boundInfo.pattern = std::move(pattern);
  boundInfo.propertyNames = parsedInfo.propertyNames;
  boundInfo.propertyTypes = std::move(propertyTypes);
  boundInfo.indexCreateFunc = indexCreateFunc;  // copy
  boundInfo.options = parsedInfo.options;
  boundInfo.ifNotExists = parsedInfo.ifNotExists;

  return std::make_unique<BoundCreateIndex>(std::move(boundInfo));
}
```

Reuses existing utility functions:
- `Binder::validateTableExistence()` (bind_ddl.cpp:217)
- `Binder::validateNodeTableType()` (bind_ddl.cpp:210)
- `Binder::validateColumnExistence()` (bind_ddl.cpp:225)
- `TableCatalogEntry::getProperty()` (table_catalog_entry.h:77)
- `PropertyDefinition::getType()` (property_definition.h:68)
- `Catalog::getFunctionEntry()` (catalog.h:236)
- `FunctionCatalogEntry::getFunctionSet()` (function_catalog_entry.h:43)

### Step 4: Update `LogicalCreateIndex` -- `include/neug/compiler/planner/operator/ddl/logical_create_index.h`

Remove standalone `CreateIndexInfo` struct, use `BoundCreateIndexInfo` directly:

```cpp
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
```

### Step 5: Simplify `appendCreateIndex` -- `src/compiler/planner/plan/append_simple.cpp`

```cpp
void Planner::appendCreateIndex(const BoundStatement& statement,
                                LogicalPlan& plan) {
  auto& boundCreateIndex = statement.constCast<BoundCreateIndex>();
  auto info = const_cast<BoundCreateIndex&>(boundCreateIndex).moveInfo();
  auto op = std::make_shared<LogicalCreateIndex>(std::move(info));
  plan.setLastOperator(std::move(op));
}
```

### Step 6: Update `GDDLConverter::convertCreateIndex` -- `src/compiler/gopt/g_ddl_converter.cpp`

Key changes:
- Get table name from `pattern->getSingleEntry()->getName()`
- `set_create_index_type()` with `indexCreateFunc.name`
- Add `property_types` conversion via `typeConverter.convertSimpleLogicalType()`

```cpp
void GDDLConverter::convertCreateIndex(const planner::LogicalCreateIndex& op,
                                       ::physical::PhysicalPlan* plan) {
  const auto& info = op.getInfo();
  auto physical_opr = std::make_unique<::physical::PhysicalOpr>();
  auto* ci = physical_opr->mutable_opr()->mutable_create_index();

  ci->set_name(info.indexName);

  // Get table name from pattern
  ci->mutable_vertex_type()->set_name(
      info.pattern->getSingleEntry()->getName());

  // Function name from TableFunction
  ci->set_create_index_type(info.indexCreateFunc.name);

  for (const auto& prop : info.propertyNames)
    ci->add_properties(prop);

  // NEW: property_types
  for (const auto& lt : info.propertyTypes) {
    auto irType = typeConverter.convertSimpleLogicalType(lt);
    *ci->add_property_types() = std::move(*irType->mutable_data_type());
  }

  for (const auto& [k, v] : info.options)
    (*ci->mutable_options())[k] = v;

  ci->set_conflict_action(info.ifNotExists
      ? ::physical::ON_CONFLICT_DO_NOTHING
      : ::physical::ON_CONFLICT_THROW);

  plan->mutable_plan()->AddAllocated(physical_opr.release());
}
```

### Step 7: Engine Builder -- `src/execution/execute/ops/ddl/create_index.cc`

Do NOT modify. If proto field rename causes compile errors, comment out the broken lines with `// TODO: update after proto change`.

### Step 8: Update Compiler Tests -- `tests/compiler/create_index_test.cpp`

Tests need mock setup because:
1. Binder now validates table/properties against catalog
2. Binder looks up `CREATE_HNSW_INDEX` function from catalog
3. Vector index use case needs ARRAY type properties (not supported in CREATE TABLE DDL)

#### 8a. Mock approach

In the test fixture `SetUp()`, after loading schema:
1. Register a node table with ARRAY type property via catalog API
2. Register a `CREATE_HNSW_INDEX` TableFunction in catalog

```cpp
class CreateIndexTest : public GOptTest {
 public:
  std::string schemaData = getGOptResource("schema/create_follows_schema.yaml");
  std::string statsData = getGOptResource("stats/create_follows_stats.json");
  std::vector<std::string> rules = {"FilterPushDown", "ExpandGetVFusion"};

  void SetUp() override {
    GOptTest::SetUp();
    // Load base schema
    database->updateSchema(schemaData);
    database->updateStats(statsData);

    auto* catalog = getCatalog();
    auto& tx = neug::Constants::DEFAULT_TRANSACTION;

    // 1. Register node table with ARRAY type property
    //    "VectorNode" with property "vec" of type ARRAY(FLOAT, 128)
    //    Use catalog->createTableEntry() with a BoundCreateTableInfo
    //    that includes an ARRAY property definition.

    // 2. Register CREATE_HNSW_INDEX function
    function::function_set funcSet;
    auto func = std::make_unique<function::TableFunction>(
        "CREATE_HNSW_INDEX", std::vector<common::LogicalTypeID>{});
    funcSet.push_back(std::move(func));
    catalog->addFunction(&tx,
        catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY,
        "CREATE_HNSW_INDEX", std::move(funcSet));
  }

  std::string getDDLResource(std::string resource) {
    return getGOptResource("ddl_test/" + resource);
  }
};
```

#### 8b. Test queries

Use the mock VectorNode table with ARRAY property:

```cpp
TEST_F(CreateIndexTest, CREATE_INDEX_BASIC) {
  std::string query =
      "CREATE INDEX vec_hnsw_index ON VectorNode USING HNSW (vec);";
  auto logical = planLogical(query);  // schema already loaded in SetUp
  auto physical = planPhysical(*logical);
  VerifyFactory::verifyPhysicalByJson(
      *physical, getDDLResource("CREATE_INDEX_BASIC_physical"));
}
```

Also keep tests using existing User table properties (prop_int64, prop_text) for non-vector cases.

#### 8c. Expected physical plan files -- `tests/compiler/resources/ddl_test/`

Update each file:
- `"index_type":"HNSW"` -> `"create_index_type":"CREATE_HNSW_INDEX"`
- Add `"property_types"` field with actual DataType from catalog
- Update property names to match test queries

Example `CREATE_INDEX_BASIC_physical`:
```json
{"plan":[{"opr":{"create_index":{"name":"vec_hnsw_index","vertex_type":{"name":"VectorNode"},"create_index_type":"CREATE_HNSW_INDEX","properties":["vec"],"property_types":[{"array":{"component_type":{"primitive_type":"DT_FLOAT"},"max_length":128}}],"options":{},"conflict_action":"ON_CONFLICT_THROW"}},"meta_data":[]}]}
```

### Step 9: Python E2E Tests -- `tools/python_bind/tests/test_hnsw_index.py`

Python tests will fail at Binder stage because `CREATE_HNSW_INDEX` function is not registered in catalog (zvec extension does not register it yet). Options:
- Comment out or mark `@pytest.mark.skip` until extension is updated
- Or have zvec extension register a placeholder TableFunction

For now, skip these tests with a comment explaining the dependency.

## File List

| File | Action | Description |
|------|--------|-------------|
| `proto/cypher_ddl.proto` | Modify | Rename field + add property_types |
| `include/neug/compiler/binder/ddl/bound_create_index.h` | Rewrite | BoundCreateIndexInfo with pattern + TableFunction |
| `src/compiler/binder/bind/bind_ddl.cpp` | Modify | bindCreateIndex with catalog validation + function lookup |
| `include/neug/compiler/planner/operator/ddl/logical_create_index.h` | Modify | Use BoundCreateIndexInfo |
| `src/compiler/planner/plan/append_simple.cpp` | Modify | Simplify appendCreateIndex |
| `src/compiler/planner/operator/logical_create_index.cpp` | Modify | getExpressionsForPrinting uses pattern |
| `src/compiler/gopt/g_ddl_converter.cpp` | Modify | New proto fields, read from pattern |
| `src/execution/execute/ops/ddl/create_index.cc` | Comment out | If compile errors from proto change |
| `tests/compiler/create_index_test.cpp` | Rewrite | Mock ARRAY type + CREATE_HNSW_INDEX function |
| `tests/compiler/resources/ddl_test/CREATE_INDEX_*_physical` | Modify | Updated expected JSON |

## Verification

```bash
# 1. Build
cd tools/python_bind
BUILD_TEST=ON BUILD_TYPE=DEBUG make build

# 2. C++ unit tests
cd build/neug_py_bind
TEST_RESOURCE=../../tests/compiler ctest -R gopt_test --gtest_filter='CreateIndexTest.*' -V

# 3. DDL tests (ensure existing DDL tests still pass)
TEST_RESOURCE=../../tests/compiler ctest -R gopt_test --gtest_filter='GOptDDLTest.*' -V
```
