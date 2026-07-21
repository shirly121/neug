# 图 + 向量 + 全文检索 POC

NeuG 将图数据库、向量检索和全文检索能力统一融合到 Cypher 查询体系中。

通过 NeuG，用户可以在同一个查询中组合：

- 图结构查询：基于实体关系发现候选对象；
- 向量检索：基于 embedding 进行语义相似搜索；
- 全文检索：基于关键词和文本相关性搜索；
- 混合检索：结合文本相关性和向量相似度提升搜索质量。

## 1. 向量检索

### 1.1 用户场景

以云产品知识图谱为例，用户希望用自然语言提问“Flink 如何写入 MaxCompute”，找到
语义最相关的知识实体，并进一步了解这些实体属于哪个产品、与其他实体有什么关系。

知识图谱中包含两类节点：

- `Entity`：功能、配置项、连接器和产品知识等可检索实体。
- `Product`：Flink、MaxCompute 等产品节点。

实体之间、实体与产品之间通过统一关系表连接，`rel_type` 用来区分 200 多种业务关系。

```cypher
CREATE NODE TABLE Entity(
    uid STRING PRIMARY KEY,
    name STRING,
    description STRING,
    entity_type STRING,
    product STRING,
    authority INT64,
    kg_id STRING,
    embedding FLOAT[512]
);

CREATE NODE TABLE Product(
    name STRING PRIMARY KEY,
    uid STRING,
    description STRING
);

CREATE REL TABLE rel_ee(
    FROM Entity TO Entity,
    rel_type STRING,
    content STRING
);

CREATE REL TABLE rel_ep(
    FROM Entity TO Product,
    rel_type STRING,
    content STRING
);

CREATE REL TABLE rel_pe(
    FROM Product TO Entity,
    rel_type STRING,
    content STRING
);

CREATE REL TABLE rel_pp(
    FROM Product TO Product,
    rel_type STRING,
    content STRING
);
```

应用可以使用 embedding 模型对 `name + description` 编码，将结果保存为 512 维向量。
查询时使用同一个模型对用户问题编码。示例数据使用归一化后的中文 embedding，因此
可以用 Inner Product 衡量语义相似度。

### 1.2 导入向量数据

NeuG 可以从 CSV、JSON/JSONL 和 Parquet 数据中导入数组属性。以下示例从预处理后的
JSONL 文件导入实体及其 embedding：

jsonl 数据示例：

```jsonl
{
  "uid": "aliyun/DataWorks/API网关",
  "name": "API网关",
  "description": "API托管服务，提供API发布、管理、运维、售卖全生命周期管理，地域须与DataWorks工作空间一致（别名: API网关（API Gateway））",
  "entity_type": "Product",
  "product": "DataWorks",
  "authority": 3,
  "kg_id": "aliyun/DataWorks",
  "embedding": [
    -0.0444452203810215,
    -0.0207230132073164,
    0.03195725753903389,
    ...
  ]
}
```

```cypher
COPY Entity FROM (
    LOAD FROM 'entities_with_embedding.jsonl'
    RETURN uid,
           name,
           description,
           entity_type,
           product,
           authority,
           kg_id,
           CAST(embedding, 'FLOAT[512]')
);
```

这使用户可以复用现有 embedding 生成流程，一次性将实体属性和向量导入知识图谱，
不需要为向量维护独立的数据系统。

### 1.3 创建和删除 HNSW 索引

为 `Entity.embedding` 创建 HNSW 索引：

```cypher
CREATE INDEX entity_embedding_hnsw
ON Entity USING HNSW (embedding)
WITH (
    metric = 'ip',
    m = 16,
    ef_construction = 200
);
```

创建索引时，NeuG 会自动为已经导入的实体建立索引；之后新写入的实体也会自动进入
索引。因此用户既可以“先导入、后建索引”，也可以“先建索引、持续写入”。

NeuG 支持以下相似度计算方式：

| `metric` | 查询函数 | 排序方向 | 适用场景 |
| --- | --- | --- | --- |
| `ip` | `vector_distance_ip` | DESC | 已归一化的文本 embedding |
| `cosine` | `vector_distance_cosine` | ASC | 使用余弦距离比较向量方向 |
| `l2` | `vector_distance_l2` | ASC | 使用欧氏距离比较向量位置 |

索引的 metric 应与查询函数一致。对于本例中已归一化的 embedding，使用 `ip` 和
`vector_distance_ip`。

删除索引：

```cypher
DROP INDEX entity_embedding_hnsw;
```

向量只需要作为 `Entity` 属性保存一份。建立索引不会要求用户再维护一套向量副本，
能够降低大规模知识库的存储和运维成本。

### 1.4 向量相似度搜索

应用先将“Flink 如何写入 MaxCompute”编码成 512 维 `query_embedding`，再将该数组
放入下面查询的 `<query_embedding>` 位置：

```cypher
MATCH (n:Entity)
RETURN n.uid,
       n.name,
       n.description,
       n.entity_type,
       n.product,
       n.authority,
       vector_distance_ip(n.embedding, <query_embedding>) AS score
ORDER BY score DESC
LIMIT 20;
```

查询返回与问题语义最相关的 20 个实体。由于 embedding 已归一化，Inner Product
分数越高表示语义越相似。用户不需要手写数组遍历或 `reduce()` 计算点积，也不需要
调用独立的向量查询接口；向量搜索直接使用普通 Cypher 的 `MATCH`、`ORDER BY` 和
`LIMIT` 表达。

如果业务使用未归一化向量，也可以建立 `cosine` 或 `l2` 索引，并分别使用
`vector_distance_cosine` 或 `vector_distance_l2` 查询。

### 1.5 图 + 向量混合检索

#### 1.5.1 向量召回后继续图查询

向量搜索返回的是普通 `Entity` 节点，可以继续参与图模式匹配。例如先找出与用户问题
最相关的 20 个实体，再返回这些实体关联的产品和关系说明：

```cypher
MATCH (n:Entity)
WITH n,
     vector_distance_ip(n.embedding, <query_embedding>) AS score
ORDER BY score DESC
LIMIT 20
MATCH (n)-[r:rel_ep]->(p:Product)
RETURN n.uid,
       n.name,
       score,
       p.name AS related_product,
       r.rel_type,
       r.content
ORDER BY score DESC;
```

这让用户不仅得到“哪些实体与问题相似”，还可以通过知识图谱解释实体属于哪个产品、
它如何与其他产品协作。向量召回结果不会脱离图数据成为一组孤立 ID。

#### 1.5.2 标量过滤 + 向量检索

用户经常只希望搜索特定产品、实体类型或权威等级的数据。例如只搜索 MaxCompute
产品下、权威等级不低于 2 的实体：

```cypher
MATCH (n:Entity)
WHERE n.product = 'MaxCompute' AND n.authority >= 2
RETURN n.uid,
       n.name,
       n.description,
       n.authority,
       vector_distance_ip(n.embedding, <query_embedding>) AS score
ORDER BY score DESC
LIMIT 20;
```

NeuG 返回的是满足过滤条件后的 Top-20，而不是先取得全局 Top-20 再删除不符合条件
的结果。这一点对知识库权限、数据来源、产品范围等强过滤场景很重要：即使全局最相似
的实体大多属于其他产品，用户仍能得到当前允许范围内数量充足、相关性最高的结果。

#### 1.5.3 图查询结果内的向量检索

NeuG 也可以先通过图关系确定候选范围，再在这个范围内执行向量检索。例如只搜索与
MaxCompute 产品相连的实体：

```cypher
MATCH (n:Entity)-[r:rel_ep]->(p:Product {name: 'MaxCompute'})
RETURN n.uid,
       n.name,
       n.description,
       r.rel_type,
       r.content,
       vector_distance_ip(n.embedding, <query_embedding>) AS score
ORDER BY score DESC
LIMIT 20;
```

图关系可以表达比单个属性更丰富的候选条件。用户可以先限定产品依赖、上下游关系或
知识来源，再按语义相似度选出 Top-K，从而把结构化知识和非结构化语义统一在一次
Cypher 查询中。

### 1.6 自动维护向量索引

向量索引会随图数据自动更新，无需在每次数据变化后重建：

- `CREATE` 新实体时，新向量可以立即参与检索。
- `SET` 更新 embedding 后，后续查询使用新向量。
- `DELETE` 实体后，该实体不会再出现在向量结果中。

例如，更新 Flink Connector 的 embedding：

```cypher
MATCH (n:Entity {uid: 'aliyun/DataWorks/Flink_Connector'})
SET n.embedding = <updated_embedding>;
```

更新完成后可以直接执行查询，无需再次创建索引：

```cypher
MATCH (n:Entity)
RETURN n.uid,
       n.name,
       vector_distance_ip(n.embedding, <query_embedding>) AS score
ORDER BY score DESC
LIMIT 20;
```

这对持续更新的产品知识库非常重要。新文档、纠正后的描述和下线内容能够及时反映在
搜索结果中，同时避免频繁全量重建带来的服务中断。

### 1.7 索引持久化

向量索引会随数据库 checkpoint 或关闭操作持久化。重新打开数据库后，可以直接继续
执行向量查询，不需要重新生成 embedding 或重新创建索引。

索引采用磁盘存储，不要求全部索引数据常驻内存，适合向量规模持续增长的知识库。
持久化还保证批量导入和建索引的成本只需承担一次，服务重启后能够快速恢复查询能力。

## 2. 全文检索

### 2.1 用户场景

在云产品知识图谱中，用户除了使用自然语言进行语义搜索，还需要按产品名、功能名和
描述中的原始关键词查找实体。例如：

- 搜索名称中包含 `Flink` 的实体；
- 搜索名称或描述中出现 `MaxCompute` 的知识；
- 使用完整短语 `"Flink Connector"` 查找明确提到该组件的实体；
- 只在指定产品、权威等级或图关系范围内搜索关键词。

全文检索适合处理产品名、配置项、错误码和专有术语等必须精确出现的内容。它与向量
检索互补：全文检索保证关键词匹配，向量检索发现表达不同但语义相近的内容。

NeuG 使用 `Entity` 的 `name` 和 `description` 等 `STRING` 属性保存原始文本。用户可以
直接在普通 Cypher `MATCH` 查询中使用 `bm25()`，不需要把图数据导出到独立的搜索
系统。

### 2.2 创建和删除全文索引

如果只需要搜索实体描述，可以创建单列全文索引：

```cypher
CREATE INDEX entity_description_fts
ON Entity USING FTS (description);
```

如果希望同时搜索实体名称和描述，可以创建多列全文索引：

```cypher
CREATE INDEX entity_text_fts
ON Entity USING FTS (name, description)
WITH (
    name_weight = 8.0,
    description_weight = 2.0
);
```

多列权重用于表达不同属性对搜索结果的重要程度。在知识图谱中，关键词出现在实体名称
中通常比只出现在描述中更重要，因此可以为 `name` 设置更高权重。

创建索引时，NeuG 会自动为已经存在的实体建立索引；之后新增的实体也会自动进入
索引。用户既可以先批量导入知识数据再创建索引，也可以先创建索引后持续写入数据。

删除索引：

```cypher
DROP INDEX entity_text_fts;
```

### 2.3 `bm25()` 相关性查询

`bm25()` 返回实体文本与查询词的相关性分数，分数越小表示越相关。全文 Top-K 使用
普通 Cypher 的 `MATCH`、`ORDER BY` 和 `LIMIT` 表达。

单列查询形式：

```text
bm25(string_property, query_string) -> DOUBLE
```

多列查询形式：

```text
bm25([string_property, ...], query_string) -> DOUBLE
```

查询使用的属性应与全文索引一致。多列查询的属性集合和顺序应与创建索引时保持一致。

### 2.4 单列全文查询

以下查询在实体描述中搜索关键词 `MaxCompute`，返回相关性最高的 20 个实体：

```cypher
MATCH (n:Entity)
RETURN n.uid,
       n.name,
       n.description,
       n.entity_type,
       n.product,
       n.authority,
       bm25(n.description, 'MaxCompute') AS score
ORDER BY score ASC
LIMIT 20;
```

单列索引适合搜索目标明确的属性。例如只搜索描述可以避免实体名称或其他文本属性影响
排序，也可以降低不需要检索的文本带来的索引开销。

### 2.5 多列全文查询

以下查询同时搜索实体名称和描述：

```cypher
MATCH (n:Entity)
RETURN n.uid,
       n.name,
       n.description,
       n.entity_type,
       n.product,
       n.authority,
       bm25([n.name, n.description], 'MaxCompute') AS score
ORDER BY score ASC
LIMIT 20;
```

结果使用建索引时配置的列权重进行统一排序。对于产品知识搜索，名称中的精确命中可以
排在仅在长描述中偶然提到关键词的实体之前，从而提高结果的可用性。

如果只需要查询 `description`，应使用对应的单列索引；多列查询则使用完整的
`[n.name, n.description]` 属性列表。

### 2.6 短语查询

查询字符串中的双引号表示完整短语，短语中的词需要按相同顺序连续出现。以下查询查找
明确提到 `Flink Connector` 的实体：

```cypher
MATCH (n:Entity)
RETURN n.uid,
       n.name,
       n.description,
       bm25([n.name, n.description], '"Flink Connector"') AS score
ORDER BY score ASC
LIMIT 20;
```

短语查询适合产品全名、组件名和固定术语，可以避免将两个词分散出现在不同位置的文本
误判为精确命中。

### 2.7 查询表达式组合

查询字符串支持使用 `AND`、`OR`、`NOT` 和括号组合多个检索条件。例如，查找同时包含
`Flink` 和 `MaxCompute` 的实体：

```cypher
MATCH (n:Entity)
RETURN n.uid,
       n.name,
       n.description,
       bm25([n.name, n.description], 'Flink AND MaxCompute') AS score
ORDER BY score ASC
LIMIT 20;
```

也可以使用括号表达更复杂的搜索意图。例如，要求结果包含 `MaxCompute`，并且同时包含
`Flink` 或 `Connector`：

```cypher
MATCH (n:Entity)
RETURN n.uid,
       n.name,
       n.description,
       bm25(
           [n.name, n.description],
           '(Flink OR Connector) AND MaxCompute'
       ) AS score
ORDER BY score ASC
LIMIT 20;
```

`AND`、`OR` 和 `NOT` 裸写时表示查询运算符。如果需要搜索文档中的这些普通单词，使用
双引号包裹。例如，`'"AND"'` 表示搜索单词 `AND`，而不是执行逻辑与操作。

表达式组合适合将产品、组件和操作等多个条件组织成一次检索，同时仍然使用 BM25 对
满足整个表达式的实体进行相关性排序。

### 2.8 前缀查询

在词项后添加 `*` 可以搜索以该词项开头的所有 term。例如，以下查询可以匹配包含
`vector`、`vectors` 或 `vectorized` 等 term 的实体：

```cypher
MATCH (n:Entity)
RETURN n.uid,
       n.name,
       n.description,
       bm25([n.name, n.description], 'vect*') AS score
ORDER BY score ASC
LIMIT 20;
```

前缀也可以参与查询表达式组合。例如，查找包含以 `Flink` 开头的 term，同时包含
`MaxCompute` 的实体：

```cypher
MATCH (n:Entity)
RETURN n.uid,
       n.name,
       n.description,
       bm25([n.name, n.description], 'Flink* AND MaxCompute') AS score
ORDER BY score ASC
LIMIT 20;
```

前缀查询匹配的是文本分词后 term 的开头，而不是任意位置的字符串片段。结果仍可通过
`bm25()` 计算相关性，并按照分数升序返回最相关的实体。

### 2.9 全文召回后继续图查询

全文检索返回的是普通 `Entity` 节点，可以继续参与图模式匹配。例如先按关键词找出
20 个最相关实体，再返回它们关联的产品和关系说明：

```cypher
MATCH (n:Entity)
WITH n,
     bm25([n.name, n.description], 'MaxCompute') AS score
ORDER BY score ASC
LIMIT 20
MATCH (n)-[r:rel_ep]->(p:Product)
RETURN n.uid,
       n.name,
       score,
       p.name AS related_product,
       r.rel_type,
       r.content
ORDER BY score ASC;
```

用户不仅能看到关键词命中的实体，还可以立即获得实体所属产品、数据流向或依赖关系，
从而用图结构补充搜索结果的业务上下文。

### 2.10 标量过滤 + 全文检索

用户可以先按实体属性限定搜索范围。例如只搜索 Flink 产品下、权威等级不低于 2 的
知识：

```cypher
MATCH (n:Entity)
WHERE n.product = 'Flink' AND n.authority >= 2
RETURN n.uid,
       n.name,
       n.description,
       n.authority,
       bm25([n.name, n.description], 'MaxCompute') AS score
ORDER BY score ASC
LIMIT 20;
```

NeuG 返回的是满足过滤条件后的 Top-20，而不是先取得全局 Top-20 再删除不符合条件
的实体。这对权限等级、知识来源、产品范围等强过滤条件十分重要，能够避免过滤后结果
数量不足，并保证返回的是允许范围内相关性最高的实体。

### 2.11 图查询结果内的全文检索

NeuG 可以先通过图关系确定候选范围，再在该范围内进行全文检索。例如只搜索与
MaxCompute 产品相连的实体：

```cypher
MATCH (n:Entity)-[r:rel_ep]->(p:Product {name: 'MaxCompute'})
RETURN n.uid,
       n.name,
       n.description,
       r.rel_type,
       r.content,
       bm25([n.name, n.description], 'Flink') AS score
ORDER BY score ASC
LIMIT 20;
```

图关系能够表达产品依赖、数据流向和上下游关系等文本属性无法完整表达的条件。用户可
以先用图关系确定业务范围，再用关键词选出最相关的知识实体。

### 2.12 全文 + 向量混合检索

全文检索和向量检索可以在同一个 Cypher 查询中组合。例如先用关键词召回 200 个候选
实体，再根据用户问题的语义向量从候选中选出最相似的 20 个：

```cypher
MATCH (n:Entity)
WITH n,
     bm25([n.name, n.description], 'MaxCompute') AS text_score
ORDER BY text_score ASC
LIMIT 200
RETURN n.uid,
       n.name,
       n.description,
       text_score,
       vector_distance_ip(n.embedding, <query_embedding>) AS vector_score
ORDER BY vector_score DESC
LIMIT 20;
```

第一阶段的关键词召回保证结果包含重要产品术语，第二阶段的向量排序理解“Flink 如何
写入 MaxCompute”这类自然语言问题的语义。两种能力结合后，既能保持专有名词的精确
性，又能处理用户问题和文档描述之间的表达差异。

### 2.13 自动维护全文索引

全文索引会随图数据自动更新，无需在每次文本变化后重建：

- `CREATE` 新实体时，新文本可以立即参与全文查询。
- `SET` 更新名称或描述后，后续查询使用新文本。
- `DELETE` 实体后，该实体不会再出现在全文结果中。

例如，更新 Flink Connector 的描述：

```cypher
MATCH (n:Entity {uid: 'aliyun/Flink/Flink Connector'})
SET n.description = 'Flink Connector 支持将结果表实时写入 MaxCompute';
```

更新后可以直接查询，无需重新创建索引：

```cypher
MATCH (n:Entity)
RETURN n.uid,
       n.name,
       bm25(n.description, 'MaxCompute') AS score
ORDER BY score ASC
LIMIT 20;
```

这使持续新增、修订和下线的产品知识能够及时反映在搜索结果中，同时避免频繁全量
重建影响在线查询。

### 2.14 索引持久化

全文索引会随数据库 checkpoint 或关闭操作持久化。重新打开数据库后，可以直接继续
执行全文查询，不需要重新创建索引。

索引采用磁盘存储，不要求全部索引数据常驻内存，适合文本规模持续增长的知识库。
持久化保证批量导入和建索引的成本只需承担一次，服务重启后能够快速恢复查询能力。

### 2.15 当前支持范围

- 支持 `STRING` 属性的单列和多列全文索引。
- 支持为多列全文索引配置列权重。
- 支持单个关键词、双引号短语和前缀查询。
- 支持使用 `AND`、`OR`、`NOT` 和括号组合查询表达式。
- 支持 `MATCH + bm25() + ORDER BY + LIMIT` 查询形式。
- 支持标量过滤、图查询范围内检索，以及检索结果继续参与图查询。
- 支持新增、更新、删除和索引持久化。
- 暂不支持邻近、模糊、同义词和拼写纠正查询。

## 功能和性能测试

### 向量检索

本测试使用相同的 `ontology_test` Entity 数据，对比 NeuG 和 Neo4j 的向量召回率、查询
延迟、初始索引创建时间、增量写入延迟和磁盘占用。

#### 测试环境和口径

- 数据集包含 41,494 个 Entity，每个 Entity 保存一个归一化的 512 维 `FLOAT` embedding。
- 从数据集的固定位置选取 10 个 Entity embedding 作为查询向量，每个查询返回 Top-20。
- 使用 NumPy 对全部向量执行精确 Inner Product 计算，以 exact Top-20 作为 ground truth，
  召回指标为 Recall@20。
- 每个查询先预热一次，再执行 10 次；整体延迟统计共包含 100 次请求。
- 查询延迟只统计数据库内部执行时间，不使用 Python 客户端 wall-clock。NeuG 读取
  `PROFILE` 返回的执行算子总耗时；Neo4j 读取服务端 summary 中的
  `result_available_after + result_consumed_after`。因此 Neo4j 的 Python driver、Bolt
  传输、本机端口转发和 Podman VM 往返时间均不计入查询延迟。
- NeuG 使用 `metric = 'ip'`，Neo4j 使用 `cosine`。由于数据已经归一化，两种计算产生
  相同的近邻排序。
- 两边 HNSW 均配置 `m = 16`、`ef_construction = 200`。Neo4j 关闭 vector
  quantization，避免因量化导致精度和存储口径不一致。
- 初始索引创建时间从提交建索引操作开始，到索引可以执行查询为止。Neo4j 的时间包含
  等待索引状态变为 `ONLINE`。
- 增量写入测试在索引创建后逐条新增 100 个包含 embedding 的 Entity，统计每次完整
  写入操作的平均值、p50 和 p95。
- 存储测试同时记录无索引 DB、建索引后完整 DB 和两者差值。数值为目录内文件逻辑大小，
  在执行增量写入前采集。

测试机器为 Apple M2 Pro、32 GiB 内存、macOS ARM64。NeuG 采用 Python embedded 模式
直接运行在宿主机；Neo4j 使用当前最新开源版本
[Neo4j Community 2026.06.0](https://neo4j.com/docs/operations-manual/current/installation/requirements/)，
运行在 ARM64 Podman VM 中，VM 配置为 6 vCPU、3.7 GiB 内存，Neo4j heap 和 page cache
各配置 1 GiB。Neo4j 查询通过本机 Bolt 连接执行。

#### 10 个查询的召回率和延迟

下表中的延迟是每个查询执行 10 次后的 p50，单位为毫秒：

| 查询 | NeuG Recall@20 | NeuG p50 | Neo4j Recall@20 | Neo4j p50 |
| --- | ---: | ---: | ---: | ---: |
| Q1 | 100% | 0.393 | 100% | 8.000 |
| Q2 | 100% | 0.413 | 100% | 5.000 |
| Q3 | 100% | 0.600 | 100% | 5.000 |
| Q4 | 100% | 0.412 | 100% | 5.000 |
| Q5 | 100% | 0.537 | 100% | 5.000 |
| Q6 | 95% | 1.212 | 100% | 4.000 |
| Q7 | 100% | 1.932 | 95% | 5.000 |
| Q8 | 100% | 0.382 | 100% | 5.500 |
| Q9 | 100% | 0.405 | 100% | 5.000 |
| Q10 | 100% | 1.327 | 100% | 4.000 |

整体结果如下：

| 系统 | 平均 Recall@20 | 查询平均延迟 | 查询 p50 | 查询 p95 |
| --- | ---: | ---: | ---: | ---: |
| NeuG | 99.5% | 0.882 ms | 0.473 ms | 2.192 ms |
| Neo4j | 99.5% | 5.400 ms | 5.000 ms | 8.050 ms |

在该数据集和配置下，两者取得相同的平均召回率。排除客户端、网络和容器往返开销后，
NeuG 的数据库内部查询 p50 约为 Neo4j 的 1/10.6。NeuG 的计时范围是执行算子树，Neo4j
的计时范围是服务端从收到查询到结果消费完成；两者都属于数据库内部计时，但服务端
暴露的统计边界并不完全相同，因此该结果适合衡量实际查询执行开销，不应解释为两种
HNSW 内核的纯算法耗时差异。

#### 索引创建和增量写入

| 系统 | 初始索引创建 | 增量写入平均值 | 增量写入 p50 | 增量写入 p95 |
| --- | ---: | ---: | ---: | ---: |
| NeuG | 18.703 s | 10.473 ms | 9.843 ms | 13.918 ms |
| Neo4j | 19.465 s | 60.773 ms | 57.372 ms | 93.327 ms |

初始构建 41,494 个向量时，两者耗时接近，Neo4j 比 NeuG 多约 4%。逐条增量写入时，
NeuG 的 p50 约为 Neo4j 的 1/5.8。增量写入数据包含一个字符串主键和一个 512 维向量，
并包含向量索引同步维护成本；Neo4j 数值同样包含 Bolt 和 VM 边界开销。

#### 索引和完整 DB 存储开销

| 系统 | 无索引完整 DB | 建索引后完整 DB | 向量索引增量 |
| --- | ---: | ---: | ---: |
| NeuG | 113.45 MiB | 123.13 MiB | 9.68 MiB |
| Neo4j | 707.88 MiB | 790.34 MiB | 82.46 MiB |

NeuG 启用 external-vector 模式后，HNSW 直接读取图存储中的 embedding，不再在索引中
重复保存原始向量。本次测得 HNSW 持久化文件为 9.50 MiB；计入 checkpoint 元数据变化
后，完整 DB 增量为 9.68 MiB，约为 Neo4j 未量化向量索引增量的 1/8.5。在包含节点、
主键、原始 embedding 和索引的完整数据库口径下，Neo4j 占用约为 NeuG 的 6.4 倍。
差异来自两个系统的向量索引设计以及节点属性、512 维向量和数据库元数据的存储格式，
不能只根据 HNSW 图文件大小推断完整数据库成本。

Neo4j 2026.06 支持使用量化进一步减少索引空间并可能提高查询速度，但会引入不同的
召回率权衡；本测试使用 `vector.quantization.type = 'none'`，确保两边均基于未量化
向量进行比较。Neo4j 向量索引的版本能力和配置项可参考
[Neo4j Vector Indexes](https://neo4j.com/docs/cypher-manual/current/indexes/semantic-indexes/vector-indexes/)。

### 全文检索
