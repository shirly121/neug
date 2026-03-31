# NeuG + ZVec 整合方案

将 ZVec 作为 NeuG 的扩展引入，让用户通过 Cypher 语句完成向量数据的管理与查询。标量数据和图结构存储在 NeuG，向量数据和索引存储在 ZVec，两者通过主键关联，对用户呈现统一视图。

用户无需关心数据的物理分布：建表、写入、更新、查询都使用同一套 Cypher 语法，系统自动将标量操作路由到 NeuG、将向量操作路由到 ZVec。整合后，用户可以在一条查询中同时利用图的关系能力和向量的相似度能力。

## 三类核心场景

- **场景一：节点上的向量属性**  
  节点可声明向量字段，用户像使用普通属性一样读写，底层自动路由到 ZVec 存储。建表时声明向量列，导入数据时标量和向量自动分流写入各自存储，对用户完全透明。

- **场景二：向量近邻搜索**  
  在图上执行「找最相似的 K 个节点」，支持多种距离度量方式：l2, cosine 等。搜索结果可直接参与后续的图遍历，反之亦然。例如，先找到与某段文本最相似的 5 个人，再沿社交关系继续探索。

- **场景三：图 + 向量混合查询**  
  先用图关系缩小范围再做向量搜索，或先向量搜索再沿图扩展，两种模式均支持。例如，先找到某人的所有二度好友，再在这些好友中按兴趣向量找最匹配的；或者先搜出最相似的候选节点，再查看它们之间的图关系。


## 用户 API

### DDL

#### 创建点类型

向量列与标量列一样出现在 `CREATE NODE TABLE` 中，类型由 ZVec extension 提供。

```cypher
CREATE NODE TABLE IF NOT EXISTS person (
    id STRING,
    name STRING,
    bio STRING,
    bio_embed VECTOR(768, FP32),
    PRIMARY KEY (id)
);
```

同一节点表可有多个向量列（名称不重复、维数与元素类型各自独立）：

```cypher
CREATE NODE TABLE person (
    id STRING,
    name STRING,
    bio_text_embed VECTOR(768, FP32),
    face_embed VECTOR(512, FP32),
    PRIMARY KEY (id)
);
```

不设 `DEFAULT` 时，向量列的「系统默认」由 ZVec extension 定义（例如 [0, 0, 0...]）。显示设置默认值 ZVec 暂时不支持，需报错。例如：

```cypher
CREATE NODE TABLE person (
    id STRING,
    name STRING,
    bio STRING,
    bio_embed VECTOR(768, FP32) DEFAULT [0.1] * 768,
    PRIMARY KEY (id)
);
```

#### 修改点类型 (ZVec 暂时不支持 Add/Drop 向量属性)

**增加向量列**（与 `ALTER TABLE … ADD IF NOT EXISTS` 一致）；extension 在 ZVec 侧创建对应 collection / 列映射：

```cypher
ALTER TABLE person ADD IF NOT EXISTS bio_embed VECTOR(768, FP32);
```

**删除向量列**（与 `ALTER TABLE … DROP IF EXISTS` 一致）；extension 同步删除 ZVec 中该列数据与依赖索引（或拒绝删除直至先 `DROP VECTOR INDEX`）：

```cypher
ALTER TABLE person DROP IF EXISTS bio_embed;
```

#### 删除点类型

与 `DROP TABLE` 语法一致。删除节点表时，ZVec extension 级联删除该表各向量列对应的 collection、索引及向量数据：

```cypher
DROP TABLE IF EXISTS person;
```

### 向量索引（Indexing）

向量索引建在 **节点表的某一向量列** 上，由 ZVec 构建（如 HNSW）；语法上与其他索引创建语法统一。

**创建索引**：

```cypher
CREATE INDEX person_bio_embed_idx  // 索引名称
ON person (bio_embed)              // 节点表与向量列
USING HNSW                         // 索引类型
WITH (m = 16, ef_construction = 200, metric = 'cosine'); // 索引参数
```

**删除索引**：

```cypher
DROP INDEX IF EXISTS person_bio_embed_idx;
```

### DML

当前优先支持 `COPY FROM` 批量导入。`CREATE`、`SET`、`DELETE` 等增量操作涉及 NeuG 与 ZVec 之间的数据版本统一，本阶段仅定义用户语法，暂缓详细设计与实现。

#### 批量加载（COPY FROM）

**先导入 schema 再导入数据**

向量数据以字符串格式存储在 CSV 文件中：
```csv
id,name,bio,bio_embed
1,marko,this is bio1,"[0.1, 0.2, 0.3]"
```

创建 Schema：
```cypher
CREATE NODE TABLE person (
    id STRING,
    name STRING,
    bio STRING,
    bio_embed VECTOR(768, FP32),
    PRIMARY KEY (id)
);
```

数据导入：
```cypher
COPY person FROM "person.csv" (header=true, delim=',');
```

>在实际导入过程中，我们遵循 "标量数据写入 NeuG，向量数据写入 ZVec，通过主键连接两部分数据" 原则，大致遵循以下流程:
>- 首先将数据源按列拆分（基于 arrow 实现），将主键+标量列分为 G0，主键+向量列为 G1。
>- 生成 UNION Subquery 结构，G0 数据 + BatchInsert 算子走 NeuG 的数据导入，G1数据 + ZVec.DocInsert 接口走 ZVec 数据导入
>- 汇总两部分数据导入结果，返回统一的状态信息


除 CSV 外，还支持 JSON/Parquet 格式，向量以数组/list 形式存储。JSON 示例：

```jsonl
{"id": "1", "name": "marko", "bio": "this is bio1", "bio_embed": [0.1, 0.2, 0.3]}
```

**无需 Schema 直接导入数据**

类型推断过程中，系统难以自动区分普通数组与向量数据，因此需要用户通过 `CAST` 显式指定向量列的目标类型：

```cypher
COPY person FROM (
    LOAD FROM 'person.csv'
    RETURN id, name, bio, CAST('bio_embed', VECTOR(768, FP32))
)
```

效果类似于先显式建表再 COPY FROM 的组合。

#### 插入节点（CREATE）

向量值可为字面列表或由构造函数包裹（便于客户端传参）：

```cypher
CREATE (p:person {
  id: 'u1',
  name: 'Alice',
  bio: 'engineer',
  bio_embed: $emb_list
});
```

其中 `$emb_list` 为长度与维度一致的列表参数（例如表声明 `VECTOR(768, FP32)` 时须 768 个元素）。若实现支持字面量列表，元素个数必须与 DDL 中的 `dim` 一致，否则编译期或执行期报错。

#### 更新（SET）

```cypher
MATCH (p:person)
WHERE p.id = 'u1'
SET p.bio = 'staff engineer', p.bio_embed = $new_emb
RETURN p.*;
```

#### 删除节点与边（DELETE / DETACH DELETE）

删除节点时，extension **级联删除** ZVec 中该主键下所有已注册向量列的数据；`DETACH DELETE` 行为不变（先删边再删点，仍触发向量级联删除）。

```cypher
MATCH (p:person)
WHERE p.id = 'u1'
DELETE p;
```

```cypher
MATCH (p:person)
WHERE p.id = 'u1'
DETACH DELETE p;
```

删除边不涉及向量列（当前方案不向边表声明向量类型）。

### DQL

通过 `CALL VECTOR_SEARCH()` 语法执行向量搜索，语法定义如下：

```cypher
CALL VECTOR_SEARCH(
  <node_variable>,              // 节点变量
  [                              // 向量查询列表
    {column: <prop_1>, vector: <vec_1>},
    {column: <prop_2>, vector: <vec_2>},
    ...
  ],
  <topk>,                        // 融合后返回的最近邻数量
  <options>                      // 多列向量搜索需包含 reranker 配置
)
```

默认输出 Schema：

| 名称 | 类型 | 描述 |
|------|------|------|
| `<node_variable>` | NODE | 匹配到的邻近节点，可通过 `n.prop` 访问标量属性，通过 `n.vec_col` 访问向量属性 |
| `score` | DOUBLE | 距离/相似度分数。单列搜索时为原始距离值（值越小越相似）；多列搜索时为 Reranker 融合后的分数（值越大越相关） |

`options` 中可选参数：

| 参数 | 类型 | 必选 | 说明 |
|------|------|------|------|
| `metric` | STRING | 否 | 距离度量：`'l2'`、`'cosine'`、`'ip'`，默认使用索引创建时指定的 metric |
| `reranker` | STRING | 多列时必选 | 多列融合策略：`'rrf'` 或 `'weighted'` |
| `weights` | MAP | `weighted` 时必选 | 各列权重，如 `{bio_embed: 0.7, face_embed: 0.3}` |
| `rank_constant` | INT | 否 | RRF 平滑常数，默认 60 |

> **设计说明**：当前采用显式 `CALL` 语法，暂不支持嵌入式语法（如 `ORDER BY n.bio_embed <-> [...]`），后者需要编译器对算子组合做特殊转换，实现复杂度较高，可在后续版本考虑。

以下列举需要支持的查询功能。

#### 单列向量搜索

**基本向量查询**

查找与向量 `[0.1, 0.2, 0.3]` 最接近的 5 个 person 以及邻近分数：

```cypher
MATCH (n:person)
CALL VECTOR_SEARCH(
  n,
  [{column: 'bio_embed', vector: [0.1, 0.2, 0.3]}],
  5
)
RETURN n.bio, score;
```

**查询向量属性**

```cypher
MATCH (n:person)
CALL VECTOR_SEARCH(
  n,
  [{column: 'bio_embed', vector: [0.1, 0.2, 0.3]}],
  5
)
RETURN n.bio, n.bio_embed, score;
```

**向量过滤**

```cypher
MATCH (n:person)
WHERE n.age > 20 AND n.name <> 'marko'
CALL VECTOR_SEARCH(
  n,
  [{column: 'bio_embed', vector: [0.1, 0.2, 0.3]}],
  5
)
RETURN n.bio, score;
```

**向量计算**

```cypher
MATCH (n:person)
WHERE n.age > 20 AND n.name <> 'marko'
RETURN n.name, ZVec.distance(n.bio_embed, [0.1, 0.2, 0.3], {metric: 'l2'});
```

```cypher
MATCH (n:person)
WHERE n.age > 20 AND n.name <> 'marko'
RETURN n.name, ZVec.distance(n.bio_embed, [0.1, 0.2, 0.3], {metric: 'cosine'});
```

```cypher
// 对向量属性执行聚合 —— ZVec 当前不支持，应返回明确的错误提示
MATCH (n:person)
RETURN AVG(n.bio_embed);
```

**向量 + 图遍历混合查询**

```cypher
// 先向量搜索，再沿图关系扩展
MATCH (n:person)
WHERE n.age > 20 AND n.name <> 'marko'
CALL VECTOR_SEARCH(
  n,
  [{column: 'bio_embed', vector: [0.1, 0.2, 0.3]}],
  5
)
MATCH (n)-[:located_in]->(c:city)
RETURN n.name, c.name, score;
```

```cypher
// 先图遍历，再对遍历结果做向量搜索
MATCH (n:person {id: '1'})-[:knows]->(f:person)
CALL VECTOR_SEARCH(
  f,
  [{column: 'bio_embed', vector: [0.1, 0.2, 0.3]}],
  5
)
RETURN f.name, f.bio_embed, score;
```

#### 多列向量搜索

**RRF 融合**（无需调参，适合快速上手）：

```cypher
MATCH (n:person)
CALL VECTOR_SEARCH(
  n,
  [{column: 'bio_embed', vector: $text_vec},
   {column: 'face_embed', vector: $face_vec}],
  10,
  {reranker: 'rrf'}
)
RETURN n.name, score;
```

**加权融合**（可精细控制各列权重）：

```cypher
MATCH (n:article)
CALL VECTOR_SEARCH(
  n,
  [{column: 'dense_embed', vector: $dense_vec},
   {column: 'sparse_embed', vector: $sparse_vec}],
  10,
  {reranker: 'weighted',
   weights: {dense_embed: 0.7, sparse_embed: 0.3},
   metric: 'cosine'}
)
RETURN n.title, score;
```

**多列搜索 + 图遍历**：

```cypher
// 先图遍历缩小范围，再多列向量搜索
MATCH (me:person {id: 'u1'})-[:knows]->(f:person)
CALL VECTOR_SEARCH(
  f,
  [{column: 'interest_embed', vector: $interest_vec},
   {column: 'skill_embed', vector: $skill_vec}],
  5,
  {reranker: 'weighted',
   weights: {interest_embed: 0.5, skill_embed: 0.5},
   metric: 'cosine'}
)
RETURN f.name, score;
```

## 设计方案

通过 ZVec Extension 支持向量属性管理与搜索功能。ZVec Extension 作为 NeuG 的动态扩展加载，在编译层注册类型、函数和 DDL 钩子，在执行层通过 ZVec SDK 完成向量数据的实际读写。

### 整体架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                         用户 Cypher 查询                             │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      NeuG 编译层 (Compiler)                         │
│                                                                     │
│  Parser ──► Binder ──► Planner ──► Optimizer ──► GPhysicalConvertor │
│               │           │           │                             │
│               │     ┌─────┴─────┐     │                             │
│               │     │ ZVec 扩展  │     │                             │
│               │     │ 介入点     │     │                             │
│               │     └───────────┘     │                             │
│               │                       │                             │
│  ┌────────────┴───────────────────────┴──────────────────────────┐  │
│  │                    Catalog (全局元数据)                         │  │
│  │  tables: NodeTableCatalogEntry (含向量列元信息)                 │  │
│  │  types:  VECTOR(dim, elem_type) ← ZVec Extension 注册         │  │
│  │  indexes: IndexCatalogEntry (HNSW 等) ← ZVec Extension 注册   │  │
│  │  functions: VECTOR_SEARCH, ZVec.distance ← ZVec Extension 注册│  │
│  └───────────────────────────────────────────────────────────────┘  │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ protobuf PhysicalPlan
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      NeuG 执行层 (Execution)                        │
│                                                                     │
│  PlanParser ──► Pipeline [ IOperator, IOperator, ... ]              │
│                     │                                               │
│         ┌───────────┼───────────────┐                               │
│         ▼           ▼               ▼                               │
│  ┌──────────┐ ┌──────────┐  ┌─────────────────┐                    │
│  │ ScanOpr  │ │SelectOpr │  │ ProcedureCallOpr │                    │
│  │ (标量列) │ │ (过滤)   │  │ (VECTOR_SEARCH)  │                    │
│  └──────────┘ └──────────┘  └────────┬────────┘                    │
│                                      │                              │
│                              ┌───────┴───────┐                      │
│                              │ ZVecCallInput  │                     │
│                              │ (bind 阶段产物) │                     │
│                              └───────┬───────┘                      │
│                                      │                              │
└──────────────────────────────────────┼──────────────────────────────┘
                                       │ ZVec SDK 调用
                                       ▼
                          ┌─────────────────────────┐
                          │    ZVec (进程内向量库)     │
                          │                         │
                          │  Collection (per 向量列) │
                          │  ├─ Vector Storage      │
                          │  ├─ HNSW / IVF Index    │
                          │  └─ Scalar Filter       │
                          └─────────────────────────┘
```

### ZVec Extension 注册内容

Extension 通过 `extern "C" Init()` 入口加载，向 NeuG 的全局 Catalog 注册以下内容：

```
ZVec Extension Init()
  │
  ├── 1. 注册类型 ──────── Catalog::createType("VECTOR", ...)
  │                         使 DDL 中 VECTOR(768, FP32) 可被 Parser/Binder 识别
  │
  ├── 2. 注册 CALL 函数 ── ExtensionAPI::registerFunction<VectorSearchFunction>
  │                         (CatalogEntryType::STANDALONE_TABLE_FUNCTION_ENTRY)
  │                         使 CALL VECTOR_SEARCH(...) 可被 ProcedureCallOpr 执行
  │
  ├── 3. 注册标量函数 ──── ExtensionAPI::registerFunction<ZVecDistanceFunction>
  │                         (CatalogEntryType::SCALAR_FUNCTION_ENTRY)
  │                         使 ZVec.distance(...) 可在 RETURN/WHERE 中使用
  │
  ├── 4. 注册 DDL 钩子 ── Catalog 层面的回调，在 CREATE/DROP TABLE 时触发
  │                         用于在 ZVec 侧创建/删除 Collection
  │
  ├── 5. 注册索引处理器 ── 使 CREATE INDEX ... USING HNSW 路由到 ZVec 建索引
  │
  └── 6. 注册扩展信息 ──── ExtensionAPI::registerExtension({name: "zvec", ...})
```

### 各用户 API 的实现路径

#### DDL：CREATE NODE TABLE（含向量列）

```
用户: CREATE NODE TABLE person (id STRING, bio_embed VECTOR(768, FP32), ...)

  Parser
    │  识别 VECTOR(768, FP32) 为 ZVec 注册的自定义类型
    ▼
  Binder
    │  解析列定义，将 VECTOR 列标记为"外部存储"类型，按照标量和向量属性拆分成两个 Create 操作
    ▼
  DDL 执行
    │  1. NeuG 侧 (CreateVertexSchema)：创建 NodeTableCatalogEntry，向量列在 NeuG 中仅保留元信息
    │     （不分配物理存储列），主键列正常存储
    │  2. ZVec 侧（CreateVectorSchema, 通过 DDL 钩子触发）：
    │     - 调用 zvec.create_and_open() 创建该节点表对应的 Collection
    │     - Collection schema 包含：主键字段 + 该表所有向量字段
    │  3. 记录映射关系（需要持久化）：table_name → Collection(zvec_collection_path)
    ▼
  Catalog 最终状态
    tables: person { id: STRING(PK), bio_embed: VECTOR(768,FP32) [external=zvec] }
    zvec_mappings: { person → Collection("./zvec_data/person/") }
```

#### DDL：CREATE INDEX ... USING HNSW

```
用户: CREATE INDEX idx ON person(bio_embed) USING HNSW WITH (m=16, ...)

  Binder
    │  识别 bio_embed 为 VECTOR 类型列
    |  将 CREATE INDEX 改写为 CreateVectorIndex (通过钩子触发)，并且构建参数列表
    ▼
  DDL 执行
    │  1. ZVec 侧：
    │     - 在对应 Collection 上调用 create_index(HnswIndexParam(...))
    ▼
  Catalog 最终状态
    indexes: person_bio_embed_idx { type: HNSW, table: person, column: bio_embed }
```

#### DDL：DROP TABLE

```
用户: DROP TABLE IF EXISTS person

  DDL 执行
    │  1. NeuG 侧：执行 DropVertexSchema 操作删除 NeuG Schema 以及数据
    │  2. ZVec 侧（DropVectorSchema, 通过 DDL 钩子触发）：
    │     - 找到该节点表对应的 Collection，调用 destroy()
    │       （一个 Collection 包含该表所有向量列数据，一次销毁即可）
    │     - 清理映射记录
```

#### DML：COPY FROM

```
用户: COPY person FROM "person.csv" (header=true, delim=',')

  Binder
    │  解析目标表 person 的 schema
    │  识别出标量列组 G0 = {id, name, bio} 和向量列组 G1 = {id, bio_embed}
    ▼
  Planner / 执行
    │
    │  ┌─── 数据源读取（Arrow RecordBatch）───┐
    │  │    CSV → Arrow，向量列解析为 LIST<FLOAT> │
    │  └──────────────┬───────────────────────┘
    │                 │
    │       ┌────────┴────────┐
    │       ▼                 ▼
    │  G0: 标量列          G1: 向量列
    │  {id, name, bio}     {id, bio_embed}
    │       │                 │
    │       ▼                 ▼
    │  NeuG BatchInsert    ZVec Collection.insert(
    │  (现有导入路径)        [Doc(id=pk, vectors={"bio_embed": [...]})]
    │       │              )
    │       ▼                 │
    │  NeuG 写入完成          ▼
    │                     ZVec 写入完成
    │       └────────┬────────┘
    │                ▼
    │         汇总结果，返回导入行数
```

#### DML：CREATE / SET / DELETE（增量操作）

```
CREATE (p:person {id: 'u1', bio_embed: $emb})
  → NeuG: 插入标量列 {id: 'u1'}
  → ZVec: collection.insert(Doc(id='u1', vectors={"bio_embed": $emb}))

SET p.bio_embed = $new_emb
  → ZVec: collection.update(Doc(id=p.id, vectors={"bio_embed": $new_emb}))
  → NeuG 标量列不受影响

DELETE p
  → NeuG: 删除节点（含标量列）
  → ZVec: collection.delete(p.id)  // DDL 钩子或级联触发
```

#### DQL：CALL VECTOR_SEARCH

这是最核心的查询路径。`VECTOR_SEARCH` 注册为 `NeugCallFunction`，通过 `ProcedureCallOpr` 执行。

```
用户: MATCH (n:person) WHERE n.age > 20 AND n.name <> 'marko'
      CALL VECTOR_SEARCH(n, [{column:'bio_embed', vector:[...]}], 5)
      RETURN n.name, score
```

**编译阶段**

```
Parser: 识别 CALL VECTOR_SEARCH(...)
  ▼
Binder: 解析参数
  - node_variable → 绑定到 MATCH 中的 n:person
  - 向量查询列表 → 校验列名存在、维度匹配
  - topk, options → 提取 reranker 等参数
  ▼
Planner: 生成逻辑计划
  LogicalScanNodeTable(person)
    → LogicalFilter(n.age > 20 AND n.name <> 'marko')
      → LogicalTableFunctionCall(VECTOR_SEARCH)
        → LogicalProjection(n.name, score)
  ▼
Optimizer: 过滤下推 + 标量条件融合
  1. 将 WHERE 条件下推到 ScanNodeTable 层
  2. 识别下推后的标量过滤条件是否可以融合到 VECTOR_SEARCH 中，
     作为 VECTOR_SEARCH 的标量过滤参数（scalar_filter）
  优化后的逻辑计划:
    LogicalTableFunctionCall(VECTOR_SEARCH,
      scalar_filter = "n.age > 20 AND n.name <> 'marko'")
      → LogicalProjection(n.name, score)
  ▼
GPhysicalConvertor: 转为 protobuf PhysicalPlan
```

**执行阶段**

`ProcedureCallOpr` 根据是否有上游算子，分两条路径执行：

**路径 A：无上游算子（标量条件已融合到 VECTOR_SEARCH）**

当 Optimizer 将所有 WHERE 条件融合到 VECTOR_SEARCH 的 `scalar_filter` 中后，Pipeline 中不再有独立的 ScanOpr 和 SelectOpr，VECTOR_SEARCH 直接作为数据源：

```
Pipeline: [ProcedureCallOpr(VECTOR_SEARCH)] → [ProjectOpr]

ProcedureCallOpr 执行流程:
  1. bindFunc (编译期):
     - 从 PhysicalPlan 中提取 VECTOR_SEARCH 参数
     - 构造 ZVecCallInput {
         table_name, columns, vectors, topk, options,
         scalar_filter = "age > 20 AND name <> 'marko'"
       }

  2. execFunc (运行期):
     - 直接调用 ZVec SDK 执行全图向量检索，标量过滤由 ZVec 在
       搜索过程中通过回调 NeuG Storage 完成:

       collection.query(
         vectors = [VectorQuery("bio_embed", vector=[...])],
         topk = 5,
         filter = "age > 20 AND name <> 'marko'",
         storage = graph,          // NeuG IStorageInterface 引用
         vertex_type = "person"    // 节点类型，用于定位标量列
       )

     - ZVec 在 ANN 搜索过程中，对每个候选节点通过 storage 回调
       读取 NeuG 标量属性（age, name），评估 filter 表达式，
       跳过不满足条件的节点
     - 返回: [(doc_id, score), ...]
     - 输出 Context: 匹配的节点 + score 列
```

**路径 B：有上游算子（图遍历或复杂过滤后的候选集）**

当 VECTOR_SEARCH 前有图遍历（如 `MATCH (a)-[:knows]->(n)`）或无法融合的复杂过滤时，上游算子先产出候选节点 ID 集合，VECTOR_SEARCH 在此集合上做 pre-filter 搜索：

```
Pipeline: [ScanOpr] → [EdgeExpandOpr] → [ProcedureCallOpr(VECTOR_SEARCH)] → [ProjectOpr]

ProcedureCallOpr 执行流程:
  1. bindFunc (编译期):
     - 构造 ZVecCallInput { table_name, columns, vectors, topk, options }

  2. execFunc (运行期):
     - 从上游 Context 获取 <node_variable> 对应的节点 ID 集合
     - 将节点主键列表传给 ZVec 作为候选集约束:

       collection.query(
         vectors = [VectorQuery("bio_embed", vector=[...])],
         topk = 5,
         filter = "pk IN ('pk1','pk2',...)"   // 上游产出的候选 ID
       )

     - 返回: [(doc_id, score), ...]
     - 输出 Context: 匹配的节点 + score 列
```

**ZVec SDK 标量过滤回调接口**

路径 A 中，ZVec 需要在向量搜索过程中访问 NeuG 的标量属性来评估过滤条件。为此需要扩展 ZVec SDK 的 `query()` 接口，增加 NeuG Storage 回调参数：

```cpp
// ZVec 扩展接口：标量属性访问回调
struct ScalarFilterContext {
    neug::IStorageInterface& graph;   // NeuG 存储接口
    std::string vertex_type;          // 节点类型（用于定位标量列）
    std::string filter_expr;          // 标量过滤表达式
};

// ZVec collection.query() 扩展签名
QueryResult Collection::query(
    const std::vector<VectorQuery>& vectors,
    int topk,
    const ScalarFilterContext* scalar_ctx = nullptr,  // 可选，路径 A 使用
    const std::string& id_filter = "",                // 可选，路径 B 使用
    const Reranker* reranker = nullptr                // 可选，多列搜索使用
);
```

ZVec 在 ANN 搜索（如 HNSW 遍历）过程中，对每个候选节点调用 `scalar_ctx->graph.GetVertexPropColumn(label, prop_name)` 读取标量属性值，评估 `filter_expr`，不满足条件的节点被跳过。

**多列向量搜索**的执行路径与单列一致，区别在于 `query()` 调用时传入多个 VectorQuery 和 Reranker：

```
单列: collection.query(vectors=[VectorQuery("bio_embed", vec)], topk=5, ...)
多列: collection.query(
        vectors=[VectorQuery("bio_embed", vec1), VectorQuery("face_embed", vec2)],
        topk=5,
        reranker=RrfReRanker() 或 WeightedReRanker(weights={...}),
        ...
      )
```

ZVec 内部完成多路搜索和融合，返回统一的 (doc_id, score) 列表。

#### DQL：ZVec.distance() 标量函数

```
用户: RETURN ZVec.distance(n.bio_embed, [0.1, 0.2, 0.3], {metric: 'cosine'})

  注册为 ScalarFunction，执行时:
    1. 从 Context 获取当前节点 n 的主键
    2. 调用 ZVec SDK: collection.fetch(pk) 获取向量数据
    3. 在 NeuG 侧（或 ZVec 侧）计算距离值
    4. 返回 DOUBLE 类型结果
```

#### DQL：向量属性读取（n.bio_embed）

```
用户: RETURN n.bio_embed

  编译阶段:
    Binder 识别 bio_embed 为 VECTOR 类型（external=zvec）
    GExprConverter 将其编码为特殊的属性访问表达式

  执行阶段:
    BindedVertexPropertyAccessor 的扩展版本:
      - 检测到属性为 ZVec 外部存储
      - 调用 ZVec SDK: collection.fetch(pk) 获取向量
      - 将向量数据转为 NeuG 的 LIST<FLOAT> 值返回
```

### ZVec 数据映射

每个节点表对应一个 ZVec Collection，表内的多个向量列作为同一 Collection 中不同的向量字段：

```
NeuG 节点表                          ZVec Collection
─────────────                        ───────────────
person (bio_embed, face_embed)  ──►  ./zvec_data/person/
article (dense_embed)           ──►  ./zvec_data/article/
```

Collection 的 Schema 由节点表的主键 + 所有向量列动态构成：

```python
# person 表有两个向量列 → Collection 包含两个向量字段
CollectionSchema(
    name="person",
    fields=[
        FieldSchema("pk", DataType.STRING)       # NeuG 主键，作为关联键
    ],
    vectors=[
        VectorSchema("bio_embed", DataType.VECTOR_FP32, dimension=768),
        VectorSchema("face_embed", DataType.VECTOR_FP32, dimension=512)
    ]
)

# article 表有一个向量列 → Collection 包含一个向量字段
CollectionSchema(
    name="article",
    fields=[
        FieldSchema("pk", DataType.STRING)
    ],
    vectors=[
        VectorSchema("dense_embed", DataType.VECTOR_FP32, dimension=768)
    ]
)
```

主键关联规则：NeuG 节点的主键值 = ZVec Doc 的 `pk` 字段值。无需额外的 ID 映射表。

这种一表一 Collection 的映射方式与 ZVec 的多向量查询能力天然契合：多列向量搜索时，多个 VectorQuery 指向同一 Collection 中的不同向量字段，ZVec 内部直接完成融合，无需跨 Collection 关联。

### 关键设计决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 向量列在 NeuG 中的存储 | 仅存元信息，不分配物理列 | 避免数据冗余，向量数据完全由 ZVec 管理 |
| NeuG ↔ ZVec 关联方式 | 主键直接映射 | NeuG 主键 = ZVec Doc pk，无需额外映射表，简单可靠 |
| 每节点表一个 Collection | 是 | 同一 Doc 承载该节点的所有向量列，多列搜索在同一 Collection 内完成，天然支持 ZVec 的多向量查询和 Reranker |
| VECTOR_SEARCH 实现方式 | NeugCallFunction + ProcedureCallOpr | 复用现有 CALL 机制，无需修改编译器核心 |
| 多列搜索的融合位置 | ZVec 侧（Reranker） | 避免 NeuG 侧多次调用和自行融合，利用 ZVec 原生能力 |
| WHERE 过滤与向量搜索的配合 | 两种路径：若上游为 Scan + 标量 Filter，则融合到 VECTOR_SEARCH 的 scalar_filter 由 ZVec 回调 NeuG Storage 执行；若上游为复杂 Pattern（图遍历、JOIN 等），则先执行上游得到候选 ID 集合，再传给 ZVec 做 pre-filter | 简单标量过滤避免多余的 Scan 开销；复杂 Pattern 利用 NeuG 图引擎能力缩小范围 |
| 向量属性读取 | 按需从 ZVec fetch | 仅在 RETURN 中引用向量列时才回取，避免不必要的 IO |
| 一致性模型 | 本阶段采用最终一致性 | COPY FROM 批量导入场景下，先写 NeuG 再写 ZVec；增量操作暂缓 |

### 需要新增/修改的 NeuG 模块

| 模块 | 变更类型 | 说明 |
|------|---------|------|
| `extension/zvec/` | **新增** | ZVec Extension 实现目录 |
| `extension/zvec/zvec_extension.cpp` | **新增** | `Init()` 入口，注册类型、函数、钩子 |
| `extension/zvec/vector_search_function.h/cpp` | **新增** | `VECTOR_SEARCH` 的 NeugCallFunction 实现 |
| `extension/zvec/zvec_distance_function.h/cpp` | **新增** | `ZVec.distance()` 标量函数实现 |
| `extension/zvec/zvec_collection_manager.h/cpp` | **新增** | 管理 NeuG 表 ↔ ZVec Collection 的映射与生命周期 |
| `extension/zvec/zvec_data_writer.h/cpp` | **新增** | COPY FROM 时向量数据写入 ZVec 的逻辑 |
| `extension/zvec/zvec_property_reader.h/cpp` | **新增** | 向量属性读取（n.bio_embed）的实现 |
| Catalog (类型注册) | **微调** | 支持 `VECTOR(dim, elem_type)` 参数化类型的解析 |
| Binder (列定义) | **微调** | 识别 VECTOR 类型列并标记为外部存储 |
| COPY FROM 流程 | **微调** | 在数据导入时按列类型分流，向量列走 ZVec 写入路径 |
| 属性访问 (VertexPropertyAccessor) | **微调** | 对 VECTOR 类型属性路由到 ZVec fetch |