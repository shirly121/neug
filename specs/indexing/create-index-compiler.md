# Compiler 侧支持 create index 功能

## 语法

```cypher
CREATE INDEX vec_hnsw_index
ON vector_node
USING HNSW (vec)
WITH (metric = 'cosine', ...);
```

## 基本结构

// parser 阶段信息
```c++
struct CreateIndexInfo {
  std::string indexName;
  std::string tableName;  // vertex label
  std::string indexType;  // "HNSW", "IVF"
  std::vector<std::string> propertyNames;
  std::unordered_map<std::string, std::string> options;  // WITH clause
  bool ifNotExists = false;
};
```

// 调用 Binder::BindCreateIndex 后：

```c++
struct BoundCreateIndexInfo {
  std::string indexName;
  // 经过 catalog 绑定后的信息，不存在 tableName 需要报错
  std::shared_ptr<binder::NodeOrRelExpression> pattern;
  // 从 pattern 中获取属性 vec，不存在需要报错
  std::vector<std::string> propertyNames;
  // 从 pattern 中获取属性 vec 类型信息
  std::vector<common::DataType> propertyTypes;
  // 从 catalog 获取指定名称的 index create function
  // 命名规则为: CREATE_to_upper(indexType)_INDEX
  function::TableFunction indexCreateFunc;
  std::unordered_map<std::string, std::string> options;  // WITH clause
  bool ifNotExists = false;
};
```

## Planner

Planner 层面基于 BoundCreateIndexInfo 构建 LogicalCreateIndex

```c++
class LogicalCreateIndex final : public LogicalOperator {
 public:
  explicit LogicalCreateIndex(BoundCreateIndexInfo info);
}
```

## Protobuf

protobuf 修改为：

```proto
// Example:
// CREATE INDEX vec_hnsw_index ON vector_node USING HNSW (vec) WITH (metric = 'cosine');
message CreateIndexOpr {
    // Unique index name
    string name = 1;
    // Vertex type the index is bound to
    common.NameOrId vertex_type = 2;
    // name of create index function
    // i.e. CREATE_HNSW_INDEX
    string create_index_type = 3;
    // Property names to index
    repeated string properties = 4;
    repeated DataType property_types = 5;
    // WITH clause options (metric, m, ef_construction, ...)
    map<string, string> options = 6;
    // Conflict handling
    ConflictAction conflict_action = 7;
}
```

## Extension

在 zvec extension 中实现 CREATE_HNSW_INDEX 函数：

```c++
struct CreateHNSWIndexFunction : public Function {
  static constexpr const char* name = "CREATE_HNSW_INDEX";

  // 实现 NeugCallFunction 中的 bindFunc 和 execFunc
  // bindFunc 创建 name, index_meta 等结构
  // execFunc 基于 name, index_meta 创建 HNSWIndex 对象
  // 执行 index->Open 和 index->Dump 操作
  // TODO: 需要插入存储中的向量数据，但目前存储还未支持向量属性，先跳过该步骤
  static function_set getFunctionSet();
};
```

## 测试

在 tests/compiler 下增加单元测试，继承 gopt_test，验证 create index 查询是否可以生成 expected physical plan。

这里需要注意的是：目前还不支持创建包含 ARRAY or LIST 类型的点或边类型，测试需要做一定 mock。
mock 方法：
- 先初始化 database
- 从 database 中获取 catalog，通过 catalog 接口直接注册包含 ARRAY 类型的点类型
- 再基于修改后 catalog 执行 planLogical 和 planPhysical