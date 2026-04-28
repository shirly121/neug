# NeuG 向量支持方案

我们希望在 NeuG 中引入向量支持能力，具体包括：

- 支持基本的向量数据类型，FP32/FP64 等
- 支持向量数据的批量导入与增量更新
- 支持向量上的距离与相似度运算：L2、cosine 等
- 支持由 HNSW 等结构加速的 KNN/TopK 检索

我们希望在 NeuG 中接入 ZVec，可以实现上述的向量功能。根据先前的调研，主要有以下两种方案：

| 方案 | Schema 操作 | 查询操作 | 写入操作 | ACID |
|------|-------------|----------|----------|------|
| 内建索引 (Indexing) | 点/边类型支持向量属性；DDL 在 NeuG 内管理 | 内部对点/边做 Scan，由 HNSW 加速 KNN/距离类查询 | 写入向量并同步维护索引 | 与 NeuG 事务一致，支持 |
| 外表 (External) | 映射为外部表/外部点表；Schema 与外部侧协调 | Scan 时回调 ZVec 查询接口，结果回填执行引擎 | `COPY` / `INSERT` / `DELETE` / `UPDATE` 经回调写入 ZVec | 不纳入 NeuG 本机事务保证 |

## 方案一：Indexing

### 向量类型
NeuG 通过 Array (定长数组) 支持向量属性类型，具体包括：

| Vector Types (ZVec 侧) | NeuG Types | 说明 |
|--------------------------|------------|------|
| `VECTOR(FP64, dim)` | `ARRAY(DOUBLE, dim)` | 双精度，与 ZVec 内部优先路径一致 |
| `VECTOR(FP32, dim)` | `ARRAY(FLOAT, dim)` | 单精度，经 FP64 或定点转换后落盘/入索引需与实现约定 |

关于类型支持的几点说明：
- **类型转换**：ZVec 统一将外部传入的向量值转换为 FP64，再将 FP64 转换为 Schema 指定的类型，我们应优先支持 FP64，剩下的类型再额外支持一层转换。
- **类型的序列化与反序列化**：为了支持 Checkpoint 操作，我们需要对向量属性值执行序列化/反序列化操作，这里需要考虑机器环境导致的 endian 问题，需要与 NeuG 其它属性协同设计。
- **Dense or Sparse**: 我们目前仅支持 Dense Vector，Sparse Vector 暂时不支持。对于向量 `[0.1, 0, 0, 0.4]`，我们仅支持 `[0.1, 0, 0, 0.4]` Dense 表示，而不支持 `{0: 0.1, 3: 0.4}` Sparse 表示。

### 用户 API

#### Schema 操作

**创建点类型 (包含向量属性)**
```cypher
// FP32、dim=4 的定长数组作为向量列；主键与向量维度在 DDL 中一次声明
CREATE NODE TABLE vector_node (id INT64, vec FLOAT[4], PRIMARY KEY (id));
```

**删除点类型**
```cypher
// 级联：该类型上的属性、依赖的 HNSW 等需按产品语义一并清理
DROP TABLE vector_node;
```

**修改点类型**

```cypher
// 新增向量列
ALTER TABLE vector_node ADD IF NOT EXISTS vec FLOAT[4];
```

```cypher
// 删列前应先 DROP 依赖该列的向量索引，否则 DDL 应报错
ALTER TABLE vector_node DROP IF EXISTS vec;
```

#### Index 操作

**创建属性索引**

```cypher
// 单列 HNSW；metric 与后续 ORDER BY / array_distance 语义需一致
CREATE INDEX vec_hnsw_index
ON vector_node
USING HNSW (vec)
WITH (metric = 'cosine');
```

**删除属性索引**
```cypher
DROP INDEX vec_hnsw_index IF EXISTS;
```

**查询属性索引**
```cypher
CALL SHOW_INDEXES();
```

#### 查询操作

**Vector Function**

我们通过 `array_distance` 族函数与辅助函数来支持向量相关运算，具体包括：

| 名称 | 描述 | 例子 |
|------|------|------|
| `array_distance_l2(a, b)` | L2（欧氏）距离：各维差方和再开方 | `array_distance_l2(n.vec, q)` |
| `array_distance_cosine(a, b)` | 余弦距离（与 metric=`cosine` 的 HNSW 配置对齐） | `array_distance_cosine(n.vec, q)` |
| `vec_normalize(a)` | 将向量 L2 归一化为单位向量，便于与 cosine 检索一致 | `vec_normalize(n.vec)` |
| `vec_slice(v, start, end)` | 对定长数组做切片视图（或拷贝），供子空间查询/降维前处理 | `vec_slice(n.vec, 0, 2)` |

```cypher
// 全表逐行算距离，未走 HNSW；适合验证或极小数据量
MATCH (n:vector_node)
RETURN array_distance_l2(n.vec, [0.1, 0.2, 0.3, 0.4]) AS d;
```

**KNN**

```cypher
// 满足 RBO/优化器条件时可改写为 HNSW 索引 Scan（见 Scan Function）
MATCH (n:vector_node)
ORDER BY array_distance_l2(n.vec, [0.1, 0.2, 0.3, 0.4])
LIMIT 3;
```

如何将上述的语法转换成 HNSW 索引查询？具体参考 [Scan Function](#scan-function) 设计

#### 写入操作

**数据导入**

vec.csv:
```
id|vec
1|"[0.1, 0.2, 0.3, 0.4]"
2|"[0.2, 0.1, 0.1, 0.1]"
...
```

```cypher
COPY vector_node FROM 'vec.csv';
```

**插入数据**

```cypher
// 新 vid 在事务提交时 Append 到 HNSW，与 IndexLocalState 中 APPEND 对应
CREATE (n:vector_node {id: 3, vec: [0.2, 0.2, 0.1, 0.1]});
```

**删除数据**
```cypher
// 物理删除对索引侧可表现为 mapping 失效或懒删除，以读路径过滤为准
MATCH (n:vector_node)
WHERE n.id = 1
DELETE n;
```

**更新数据**
```cypher
// 向量列变更对应 UPDATE_INDEX_DATA，可能为原地覆盖或先删后增（见 HNSWIndex::Update）
MATCH (n:vector_node)
WHERE n.id = 1
SET n.vec = [0.2, 0.2, 0.1, 0.1];
```

### 方案设计

#### 索引接口

```c++
// 描述索引所绑定的 label 上的列（HNSW 目前约定单列向量）
struct ColumnInfo {
    std::vector<size_t> column_ids;           // 物理列 id
    std::vector<std::string> column_names;   // 逻辑名，与 DDL 中属性名一致
    std::vector<common::LogicalTypes> column_types;  // 需与 vec 的 ARRAY(元素类型, dim) 一致
};

// 与事务/存储约定：在修改索引前由框架或 IndexManager 调用，子类可 no-op 或加锁
struct IndexLock {
    // 框架在 Append/Delete/Update/Drop 路径上于进入具体索引前调用；实现可忽略（若第三方已线程安全）
    void acquireLock(const std::mutex &lock);
};

class Index {
public:
    Index(
        const std::string &name,              // 如 'vec_hnsw_index'
        const std::string &type_name,         // 如 'HNSW'
        label_t label_id,                    // 绑定的点类型/label
        const ColumnInfo &column_info,       // 被索引的列，当前 HNSW 为单列
        case_insensitive_map_t<Value> &options);  // WITH (metric=..., ef_search=...)

    // KNN/距离下推：在可见性快照下将 vid 子集上满足条件的 top-k 写入 results
    Status Search(
        StorageReadInterface &read_transaction,  // 提供当前事务可见的顶点迭代/过滤
        IndexQueryParams params,                   // query 向量、topK、是否过滤等
        std::vector<vid_t> &results);
    // 行插入后追加索引；values 与 column_info 顺序一致
    Status Append(
        IndexLock &lock,
        vid_t vid,
        const std::vector<Property> &values);
    // 行删除时同步索引（可逻辑删或调 mapping）
    Status Delete(IndexLock &lock, vid_t vid);
    // 行更新时替换该 vid 的向量项
    Status Update(IndexLock &lock, vid_t vid, const std::vector<Property> &new_values);
    // 删除整棵索引或清空持久化
    Status Drop(IndexLock &lock);
protected:
    // 默认实现中用于多线程写保护；子类可委托给 ZVec 内部锁
    std::mutex lock;
};
```

```c++
// 每个 label 下可挂多棵索引（未来可扩展多列/多类型）
struct IndexInfo {
    label_t label_id;
    std::vector<std::unique_ptr<Index>> indexes;
};

// 统一管理所有 Index
class IndexManager {
public:
    // 创建新索引
    Status CreateIndex(const CreateIndexInfo &info);
    // 删除索引数据
    Status DropIndex(const DropIndexInfo &info);
    // 查找指定索引；*out_index 为查到的非拥有 Index*，生命周期由本管理器持有
    Status GetIndex(const GetIndexInfo &info, Index **out_index);
private:
    std::vector<IndexInfo> infos;
};
```

#### HNSW 索引实现

```c++
struct HNSWIndexLock : IndexLock {
    void acquireLock(const std::mutex &lock) {
        // ZVec IndexBridge 接口已经保证线程安全，这里不做任何处理，
        // do nothing
    }
};

// 我们额外维护一个 vertex_id <-> doc_id 的索引，可以先简单实现为 Identity
struct HNSWDocIdMap {
    uint32_t get_doc_id(vid_t vid);
    vid_t get_vertex_id(uint32 doc_id);
};

class HNSWIndex {
public:
    HNSWIndex(const std::string &name,
        const std::string &type_name,
        label_t label_id,
        const ColumnInfo &column_info,
        case_insensitive_map_t<Value> &options,
        const std::string index_path) {
        // 根据构造函数参数构建 IndexBridge 参数
        zvec_index = IndexBridge::Create(target_param);
    }

    Status Search(
        StorageReadInterface &read_transaction,
        IndexQueryParams params,
        std::vector<vid_t> &results) {
        // 从 params 获取：target query, dim, top
        // 根据 read_transaction 获取当前 ts 可见点集，并构建 ZVec::IndexFilter，传入 query_param
        // 返回 doc_ids
        zvec_index->Search(query, dimension, topk, query_param, doc_results);
        // 将 doc_ids 映射成 vid_t 返回
    }

    Status Append(
        IndexLock &lock,
        vid_t vid,
        const std::vector<Property> &values) {
        // 分配连续 doc_id，并更新 vector_doc_map
        uint32_t doc_id = allocate_doc_id(vid);
        // 从 Property 中获取 value pointer
        const void *vec = get_value_pointer(values[0]);
        zvec_index->Add(doc_id, vec);
    }

    Status Delete(IndexLock &lock, vid_t vid) {
        // 索引数据不会真正被删除，vid 被标记删除，read_transaction 不会再访问到 vid 数据，
        // 这里只是更新 mapping
        vector_doc_map.remove_vertex(vid);
    }

    Status Update(
        IndexLock &lock,
        vid_t vid,
        const std::vector<Property> &new_values) {
        uint32_t doc_id = get_doc_id(vid);
        // 这里需要进一步调研 IndexBridge::Add 接口是否允许值覆盖，也就是在 doc_id 位置上更新 new_values 值：
        // 1. 如果允许，我们可以先简单实现为覆盖
        // 2. 如果不允许，我们实现为删除+新增操作，但会导致 vid 和 doc_id 无法直接映射，需要真正维护 vector_doc_map
        Delete(lock, doc_id);
        // 为 vid 分配新 new_doc_id, new_doc_id != doc_id
        Append(lock, vid, new_values);
    }
    
    Status Drop(IndexLock &lock) {
        // 删除索引文件 index_path
    }
protected:
    IndexBridge zvec_index;
    std::string index_path;
    HNSWDocIdMap vertex_doc_map;
};
```

```c++
class IndexBridge {
 public:
  using Pointer = std::shared_ptr<IndexBridge>;

  ~IndexBridge();

  // Non-copyable
  IndexBridge(const IndexBridge&) = delete;
  IndexBridge& operator=(const IndexBridge&) = delete;

  // Movable
  IndexBridge(IndexBridge&&) noexcept;
  IndexBridge& operator=(IndexBridge&&) noexcept;

  /**
   * @brief Create a new IndexBridge for batch index building.
   * @param target_param Parameters for the target index (e.g., HNSW, IVF).
   *                     Vectors will be collected first, then batch-built.
   * @return Pointer to the bridge, or nullptr on failure.
   */
  static Pointer Create(const BaseIndexParam& target_param);

  /**
   * @brief Deserialize a previously serialized index.
   * @param param_json JSON string of index parameters.
   * @param data Serialized index data.
   * @param size Size of the serialized data.
   * @return Pointer to the bridge, or nullptr on failure.
   */
  static Pointer Deserialize(const std::string& param_json, const void* data,
                             size_t size);

  // ========== Write Operations (Collection Phase) ==========

  /**
   * @brief Add a vector to the collection (O(1) operation).
   * @param doc_id Document ID for this vector.
   * @param vector Pointer to the vector data.
   * @param dimension Dimension of the vector.
   * @return 0 on success, non-zero on error.
   */
  int Add(uint32_t doc_id, const float* vector, uint32_t dimension);

  /**
   * @brief Build the target index from collected vectors.
   *
   * This performs batch index construction using Merge, which is much faster
   * than adding vectors one by one to HNSW.
   *
   * @param concurrency Number of threads for building (0 = use default).
   * @return 0 on success, non-zero on error.
   */
  int Build(int concurrency = 0);

  // ========== Query Operations ==========

  /**
   * @brief Search the index for nearest neighbors.
   * @note Must call Build() before searching.
   */
  int Search(const float* query, uint32_t dimension, uint32_t topk,
             const BaseIndexQueryParam* query_param,
             std::vector<BridgeSearchResultItem>* results);

  // ========== Serialization ==========

  /**
   * @brief Serialize the built index to a string.
   * @note Must call Build() before serializing.
   */
  int Serialize(std::string* output);

  /**
   * @brief Get the index parameters as JSON string.
   */
  std::string GetParamJson() const;

  // ========== Metadata ==========

  uint32_t DocCount() const;
  IndexType GetIndexType() const;
  MetricType GetMetricType() const;
  uint32_t GetDimension() const;
  bool IsBuilt() const;

  // ========== Index Maintenance ==========

  int Flush();

 private:
  IndexBridge();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};
```

#### Scan Function

**RBO Rule**

我们向优化器注册一个新规则，用于将特定的 `ORDER BY array_distance... LIMIT` 转化为索引查询操作：

```c++
class HNSWIndexOptimizer {
    void rewrite(planner::LogicalPlan* plan) {
        convertTopK(...);
    };

    // MATCH ... ORDER BY array_distance*(...) LIMIT n -> 物理侧走 Index::Search
    std::shared_ptr<LogicalTableFunction> convertTopK(LogicalTopK topK) {
        (void)topK;
        auto func_set = ScanFunction::getFunctionSet();
        // scan_func 具体实现了执行函数
        auto scan_func = func_set[0];
        // 从 TopK 抽取 query 字面量/参数、距离族函数、列引用 -> IndexQueryParams
        auto scan_params = convertScanParams(
            get_index_info, index_query_params);
        return std::make_shared<LogicalTableFunction>(scan_func, scan_params);
    }
};

struct ScanFunction {
  static constexpr const char* name = "SCAN_FUNCTION";

  static function_set getFunctionSet() {
    auto func = std::make_unique<NeugCallFunction>(name, ...);
    func->bind_func = [](const neug::Schema& schema,
        const neug::execution::ContextMeta& ctx_meta,
        const ::physical::PhysicalPlan& plan,
        int op_idx) -> std::unique_ptr<CallFuncInputBase> {
        ...
        return std::make_unique<HNSWIndexScanInput>(
            get_index_info, index_query_params);
    };
    func->exec_func = [](const CallFuncInputBase& input, 
        neug::IStorageInterface& graph) {
        Index *index = nullptr;
        index_manager.GetIndex(get_index_info, &index);
        std::vector<vid> vids;
        index->Search(graph, index_query_params, vids);
        Context context;
        auto vertex_column = create_vertex_column(vids);
        context.add_column(vertex_column);
        return context;
    };
  }
}
```

#### Transaction (Before)

我们主要考虑如何在 `insert_transaction` 和 `update_transaction` 中支持索引更新操作。首先将索引相关变更归为五类（与 `IndexManager` 操作一一对应）：

| 操作类型 | 典型触发场景 |
|----------|----------------|
| `CREATE_INDEX` | `CREATE INDEX ...` DDL 成功、checkpoint 后首次加载 |
| `DROP_INDEX` | `DROP INDEX`、删列/删表导致隐式删索引 |
| `APPEND_INDEX_DATA` | `INSERT`、`COPY`、行内新顶点带向量列 |
| `DELETE_INDEX_DATA` | `DELETE` 顶点，vid 自索引中剔除 |
| `UPDATE_INDEX_DATA` | `SET` 向量列或会改变索引键的更新 |

通过 `IndexLocalState` 在一次事务中聚合上述操作：`append` 只写进事务私有缓冲区，**`commit` 时按序刷入 `index_manager`/`HNSWIndex`，`abort` 丢弃缓冲且无需对已持久化索引用 undo 日志**（以多数引擎「索引最终一致于提交点」的约定一致）。

```c++
class IndexLocalState {
    // 创建索引时调用
    void append(const CreateIndexInfo &create_index);
    // 删除索引/删除属性/删除点边类型调用
    void append(const DropIndexInfo &drop_index);
    // 数据导入/增量更新的时候调用
    void append(const AppendIndexDataInfo &append_index_data);
    // 增量删除的时候调用
    void append(const DeleteIndexDataInfo &delete_index_data);
    // 增量更新的时候调用，i.e. set n.vec = [0.1, 0.1, ...]
    void append(const UpdateIndexDataInfo &update_index_data);
    // 被 transaction::commit 调用，将本地缓存的这些状态提交到 index_manager 中；
    void commitState();
    // 被 transaction::abort 调用，清除本地缓存状态，此时还没更新到索引数据 (index_manager)，直接放弃缓存状态即可
    void clearState();
};
```

#### Transaction (Latest)

NeuG 在 AP 和 TP 模式下对 Transaction/Checkpoint/Concurrency 支持程度有所不同：

| Mode | Atomicity | Isolation | Durability |
|------|-------------|-----------|------------|
| AP | 不支持 | 独占锁 | 仅 Checkpoint |
| TP | 支持 | Read/Insert：MVCC；Update/Schema：独占锁 | WAL + Checkpoint |

此外，NeuG 当前 Transaction 上的一些限制：数据导入操作 (COPY FROM) 目前仅在 AP 模式下支持，不能保证原子性，数据写入过程中出现异常会导致当前内存数据只有部分写成功；

**Create Index**

目前仅在 AP 场景下支持 Create Index，在 Create Index 后，建议用户通过显示 `Checkpoint` 保存当前建好的索引数据。中间索引数据写坏不保证回滚，会存在部分数据成功，部分数据失败的问题。

```python
conn.execute("""
    CREATE INDEX vec_hnsw_index
    ON vector_node
    USING HNSW (vec)
    WITH (metric = 'cosine');
""")
conn.execute("Checkpoint")
```

如何基于 ZVec 实现 HNSW 索引持久化？

```c++
class HNSWIndex : Index {
public:
    HNSWIndex(const std::string &name,
        const std::string &type_name,
        label_t label_id,
        const ColumnInfo &column_info,
        case_insensitive_map_t<Value> &options,
        const std::string index_path) {
        // 以内存方式打开索引数据文件
        target_param.storage_options = kMemory;
        // 或者以 MMAP 方式打开索引数据文件，但开启 copy_on_write 机制，
        // 需要通过显示 flush/close 才能将更新数据写回到磁盘
        target_param.storage_options = kMMAP;
        target_params.copy_on_write = true;
        zvec_index = IndexBridge::Create(target_param);
    }

    Status Checkpoint(IndexLock &lock) {
        // Close or Flush 操作将内存索引数据写回到磁盘
        zec_index.Close();
    }

    Status Search(
        StorageReadInterface &read_transaction,
        IndexQueryParams params,
        std::vector<vid_t> &results);

    Status Append(
        IndexLock &lock,
        vid_t vid,
        const std::vector<Property> &values);

    Status Delete(IndexLock &lock, vid_t vid);

    Status Update(
        IndexLock &lock,
        vid_t vid,
        const std::vector<Property> &new_values);
    
    Status Drop(IndexLock &lock);
protected:
    IndexBridge zvec_index;
    std::string index_path;
    HNSWDocIdMap vertex_doc_map;
};
```

**Append Data**

```cypher
// 更新向量数据，并更新索引数据
Create (n:vector_node {id: 1, vec: [0.1, 0.1, 0.2, 0.2]});
```
该查询为 `INSERT` 模式，会进入 `INSERT_TRANSACTION`，主要流程为：
- `AddVertex`：将点数据暂时保存在当前 transaction_state，图数据还未更新
- `AddIndex`：构建索引数据保存在当前 transaction_state，索引存储还未更新
- `Abort`：出现异常后，直接丢弃 transaction_state
- `Commit`：将 transaction_state 内容正式提交到图数据库中

我们进一步展开 `Commit` 步骤：
- `WAL`：在 WAL 日志中追加 AddVertex 操作（不需要追加 AddIndex 操作，AddVertex 自动调用 AddIndex）
- `AddVertexUndo`：在插入数据前，记录 `AddVertexUndo` 日志，用于撤销修改 （未支持）
- `GraphStorage::AddVertex`：将 transaction_state 点属性数据插入到图存储中
- `IndexStorage::AddIndex`：将 transaction_state 索引数据插入到索引存储中
- `处理异常`：中间出现任何异常，调用 `AddVertexUndo`，撤销图数据更新（未支持），删除已插入的索引数据，撤销 WAL 记录 (未支持)

**Delete Data**

```cypher
// 删除向量数据，删除索引数据
MATCH (n:vector_node)
WHERE n.id = 1
DELETE n;
```

该查询为 `UPDATE` 模式，会进入 `UPDATE_TRANSACTION`，主要流程为：
- `DeleteVertexUndo`：在删除点之前记录 `DeleteVertexUndo` 日志，用于恢复删除操作
- `GraphStorage::DeleteVertex`：删除点数据，实际为标记删除，不会真正删除点数据
- `IndexStorage::DeleteIndex`：删除索引数据，在 HNSWIndex 中仅删除 vid <-> doc_id mapping，不会真正删除索引数据
- `Abort`：出现异常后，执行 `DeleteVertexUndo`，撤销属性和索引的删除操作
- `Commit`：
    - 写 WAL 日志，记录 DeleteVertex 操作；
    - Schema 相关的删除操作会放在 Commit 阶段真正生效，比如 `DropVertex/DropProperty`，如果这些操作失败，则需要通过 `DeleteVertexUndo` 回滚之前已经删除的属性和索引数据

**Update Data**

```cypher
MATCH (n:vector_node)
WHERE n.id = 1
SET n.vec = [0.1, 0.1, 0.1, 0.1]
```
该查询为 `UPDATE` 模式，会进入 `UPDATE_TRANSACTION`，主要流程为：
- `UpdateVertexUndo`：在更新点之前记录 `UpdateVertexUndo` 日志
- `GraphStorage::UpdateVertex`：emplace 修改点属性数据
- `IndexStorage::UpdateIndex`：先删除索引+新增索引，内部更新 vid <-> doc_id mapping
- `Abort`：出现异常后，执行 `UpdateVertexUndo`，撤销属性和索引的更新操作
- `Commit`：
    - 写 WAL 日志，记录 UpdateVertex 操作；
    - 其它操作失败，需要通过 `UpdateVertexUndo` 回滚之前已经更新的属性和索引数据

**Drop Index**

```cypher
DROP Index vec_hnsw_index IF EXISTS;
```

Schema 相关的删除操作会经过两个阶段：
- `Soft Delete`：在 COMMIT 以前先软删除，也就是标记删除，但并不真正删除数据，这样可以保证当前 transaction 中的其它操作可以看见当前删除操作，同时也避免 Abort 时无法恢复数据的问题，Abort 真正恢复的是这些 SoftDelete 操作
- `Hard Delete`：在 Commit 时才真正删除数据，但如果 Commit 中间发生异常，已经删除的数据是无法再恢复的

DropIndex 也采用同样的设计，具体流程如下：
- `DropIndexUndo`：先记录 `DropIndexUndo` 操作，Abort 时可以撤销软删除
- `IndexStorage::DropIndex(soft=true)`：标记删除，不真正删除索引数据
- `Abort`：出现异常后，执行 `DropIndexUndo`，撤销索引软删除
- `Commit`：
    - 写 WAL 日志，记录 DropIndex 操作
    - `IndexStorage::DropIndex(soft=false)`：hard 删除索引数据，中间任意操作失败都无法回滚，因为数据已经被删除，无法恢复

## 方案二：外表

### 外部临时表

将 attach 数据库表映射为外部临时表，通过 `LOAD FROM` 载入，不支持数据库更新和 ACID 操作。

```cypher
INSTALL zvec;
LOAD zvec;

ATTACH 'zvec.db' AS zvec_db (TYPE zvec);
LOAD FROM zvec_db.vector_node
RETURN id, vec;
```

### 外部点表

将 attach 数据库表映射为外部点表：

```cypher
INSTALL zvec;
LOAD zvec;

ATTACH 'zvec.db' AS zvec_db (TYPE zvec);
CREATE NODE TABLE zvec_db.vector_node (id INTEGER, vec VECTOR(FP64, 4));

MATCH (n:vector_node)
ORDER BY array_distance(n.vec, [0.1, 0.1, 0.2, 0.2])
LIMIT 10;
```

这个方案目前还有一些不清楚的地方：
- 需要将 SCAN/CREATE/INSERT/DELETE/UPDATE/COPY 映射为对 zvec 接口回调，需要在这些算子层面开放 extension 接口，如何支持？
- 点类型在 NeuG 中有特定实现上的约束，外部点如何实现这些接口？
    - 需要包含 vertex_id, primary_key 字段
    - 取属性操作
    - 扩展边操作
- 如何表示外部内和内部点 union 结构？
