# Tasks: SQLite FTS Index

**Input**: [`spec.md`](./spec.md)  
**实现位置**: `extension/sqlite_fts/`  
**测试原则**: 每个里程碑必须能够独立构建、独立运行、独立验收；前一里程碑通过后才能进入下一里程碑。

## 范围约束

- 实现代码默认全部新增在 `extension/sqlite_fts/` 下。
- 不修改 `StorageIndex`、`StorageIndexManager`、查询编译器等已有核心实现。
- 如果扩展接入必须修改已有构建或扩展清单文件，先列出文件、最小改动和原因，获得确认后再修改。
- 第一、第二里程碑不得引入或链接 SQLite，避免接口链路、生命周期和第三方依赖问题相互干扰。
- 当前生命周期范围只实现 `Open` 和 `Dump`；`Clone` 和 `Detach` 仅为满足 `Module` 纯虚接口而保留，并明确返回不支持错误，不在本阶段设计写时复制语义。
- 每个里程碑完成后保留对应的测试用例；后续实现不能破坏前序测试所验证的行为。
- 用户侧语法与 zvec 向量索引保持一致：使用普通 `MATCH`、评分函数、`ORDER BY score ASC` 和 `LIMIT` 触发 Top-K 优化器；内部改写为索引扫描，用户不直接调用内部扫描函数，也不传索引名称。
- 首版只支持完整的 `MATCH + SQLITE_FTS_BM25 + ORDER BY score ASC + LIMIT` 查询形态，四部分统一融合为一个索引算子；`SQLITE_FTS_BM25` 暂不支持脱离该形态单独调用。

---

## M0：扩展骨架与测试入口

**状态**: ✅ 已完成并通过 Review（2026-07-17）

**目标**: 建立 `sqlite_fts` 扩展的最小目录、模块边界和测试入口，为后续三个里程碑提供稳定的构建单元。

**独立测试**: 只构建并运行 `sqlite_fts` 扩展测试目标，确认扩展能够被加载且测试程序正常退出。

- [x] F004-T001 新增 `extension/sqlite_fts/` 目录及 `include/`、`src/`、`tests/` 子目录
- [x] F004-T002 新增扩展内部 `CMakeLists.txt`，定义扩展库和 `sqlite_fts_extension_test` 测试目标
- [x] F004-T003 新增扩展入口，注册扩展名称 `SQLITE_FTS`
- [x] F004-T004 新增 smoke test，验证 `LOAD sqlite_fts` 成功
- [x] F004-T005 记录接入根构建所需的已有文件改动；未获得确认前不执行这些改动

**验收条件**:

1. `sqlite_fts` 扩展可以单独构建。
2. `LOAD sqlite_fts` 成功。
3. 此阶段没有 SQLite 头文件、库或链接项。

---

## M1：打通 Cypher → 优化器 → 索引扫描的端到端链路

**状态**: ✅ 已完成并通过 Review（2026-07-17）

**目标**: 暂不实现全文检索，先复用 zvec 的整体框架，通过普通 Cypher 查询触发优化器，并证明查询字符串能够完整传递到目标索引实例。

**建议 Cypher**:

```cypher
CREATE INDEX item_text_fts ON Item USING SQLITE_FTS (text);
MATCH (n:Item)
RETURN n, sqlite_fts_bm25(n.text, 'search text') AS score
ORDER BY score ASC
LIMIT 10;
```

该语法与 zvec 的以下模式对齐：

```cypher
MATCH (n:Item)
RETURN n, vector_distance_l2(n.vec, [2.1, 2.1, 2.1]) AS score
ORDER BY score ASC
LIMIT 10;
```

两者都由优化器识别“属性评分函数 + ORDER BY + LIMIT”。FTS 首版会把 `MATCH`、评分、排序和截断对应的整个逻辑子树统一替换为一个 Top-K 索引算子，而不是分别执行评分函数、排序算子和 LIMIT 算子。内部算子根据 label 和 property 查找索引，不要求用户提供索引名称。SQLite FTS5 的 BM25 值越小表示相关性越高，因此与 L2 距离相同，使用 `ORDER BY score ASC`。

`SQLITE_FTS_BM25` 在首版中只是供 binder 和优化器识别的查询表达式，不提供逐行 scalar fallback。以下形态暂不支持：

```cypher
MATCH (n:Item)
RETURN sqlite_fts_bm25(n.text, 'search text');
```

缺少 `ORDER BY score ASC` 或 `LIMIT`、使用 `DESC`、存在多个排序键时，也不能进入 FTS 索引执行链路，应返回明确的“不支持”错误。

**独立测试**: 创建 stub 索引后执行上述 FTS Cypher。stub 返回确定性的候选节点和分数，同时通过测试探针断言索引收到的完整查询字符串为 `search text`、Top-K 为 `10`。

- [x] F004-T101 定义 `SQLiteFTSQueryParams`，第一版包含完整的 `query_string` 和 `topk`
- [x] F004-T102 定义不依赖 SQLite 的 `SQLiteFTSIndex` stub，并注册模块类型 `sqlite_fts_index`
- [x] F004-T103 为 stub 索引实现 ranked search，记录收到的 `SQLiteFTSQueryParams` 并返回确定性的候选 ID 和 score
- [x] F004-T104 注册供 binder/优化器识别的 `SQLITE_FTS_BM25(property, query_string)` 评分表达式，但不提供可独立执行的 scalar 实现
- [x] F004-T105 注册仅供优化器使用的融合算子 `SQLITE_FTS_INDEX_SCAN`，由该算子一次完成索引检索、BM25 评分、升序排序和 Top-K 截断
- [x] F004-T106 实现 `SQLiteFTSIndexScanOptimizer`，只识别完整的 `MATCH + sqlite_fts_bm25(n.property, query_string) AS score + ORDER BY score ASC + LIMIT k`
- [x] F004-T107 从评分表达式中提取 label、property、完整 query string 和 topk，构造 `IndexScanBindData`
- [x] F004-T108 用单个 `SQLITE_FTS_INDEX_SCAN` 替换 MATCH 扫描、评分投影、ORDER BY 和 LIMIT 对应的完整逻辑子树
- [x] F004-T109 在内部扫描执行阶段按 label/property 从 `StorageIndexManager` 获取索引
- [x] F004-T110 校验目标索引为 `SQLiteFTSIndex`，构造 `SQLiteFTSQueryParams` 并调用 ranked search 接口
- [x] F004-T111 将 ranked search 返回值构造成节点列和 score 列，并将原投影中的 `sqlite_fts_bm25` 表达式替换为内部 score 列
- [x] F004-T112 添加表达式识别测试，覆盖普通字符串、空字符串、空格和引号
- [x] F004-T113 添加优化器测试，验证合法的完整 Top-K 查询融合为一个算子；独立调用评分函数、DESC、缺少 ORDER BY、缺少 LIMIT、多排序键等形态返回明确的不支持错误
- [x] F004-T114 添加错误测试：参数类型错误、索引不存在、索引类型不匹配
- [x] F004-T115 添加端到端测试：`LOAD`、建表、`CREATE INDEX`、普通 `MATCH` 查询、节点顺序与 score 断言

**验收条件**:

1. 用户查询中不出现索引名，也不直接调用 `SQLITE_FTS_INDEX_SCAN`。
2. 优化器根据 `n.text` 的 label/property 自动找到对应的 `SQLiteFTSIndex`。
3. 查询字符串和 LIMIT 未经错误改写地到达索引实例。
4. explain/physical plan 中，MATCH 扫描、BM25 评分、ORDER BY 和 LIMIT 只对应一个融合后的 `SQLITE_FTS_INDEX_SCAN` 算子。
5. 查询结果同时包含普通节点列和 BM25 score 列，按 score 升序排列且最多返回 Top-K 条。
6. `SQLITE_FTS_BM25` 独立调用不会退化为逐行计算，而是返回明确的不支持错误。
7. 后续 `RETURN` 和属性访问无需感知 FTS 索引。
8. 错误路径返回明确错误，不崩溃。
9. 此阶段仍不包含 SQLite 依赖。

---

## M2：实现 Open/Dump 与最小文件持久化

**目标**: 在不引入 SQLite 的情况下验证索引文件的创建、Dump、checkpoint 持久化和重新打开流程。`SQLiteFTSIndex` 遵循 `Module` 的 `Open`/`Dump` 生命周期，不另外对外提供 `Close` 接口。

**生命周期占位文件**: M2 只用固定测试内容模拟文件读取和写入，验证 runtime 文件的创建、复制和 checkpoint 提交；不实现格式解析、版本检查或虚拟存储后端。M3 直接在同一路径接入 SQLite。

**独立测试**: `Open → 写入 payload → Dump → 新实例 Open → 读取 payload`，断言数据一致。生命周期实现方式与 zvec/HNSW 对齐：`Dump` 负责 flush 和 checkpoint 提交，后端句柄由索引析构释放。

- [x] F004-T201 对齐 zvec/HNSW 的 `Open`/`Dump` 生命周期，以后端句柄和 runtime path 表示已打开状态，不额外引入公开状态机接口
- [x] F004-T202 实现首次 `Open`：在 checkpoint runtime 目录创建唯一索引文件
- [x] F004-T203 实现恢复 `Open`：从 `ModuleDescriptor` 获取已持久化文件并复制到可写 runtime 文件
- [x] F004-T204 实现与 zvec 对齐的 Dump container，在 `Dump` 时刷新后端内容
- [x] F004-T205 完成 `Dump` 的 checkpoint 提交：将 runtime 文件提交到 checkpoint snapshot
- [x] F004-T206 在模块描述中保存索引文件路径
- [x] F004-T207 在析构中释放后端文件句柄；析构不代替 checkpoint 持久化
- [x] F004-T208 添加空索引的 Open/Dump 测试
- [x] F004-T209 添加重复 Dump 测试
- [x] F004-T210 添加 Open、Dump、新实例重开和再次 Dump 的文件生命周期测试
- [x] F004-T211 与 zvec 保持一致：descriptor 路径不存在时按新索引打开
- [x] F004-T212 添加多个索引实例的文件隔离测试

**验收条件**:

1. 首次创建和从 checkpoint 恢复使用同一套 `Open` 接口。
2. `Dump` 可以重复调用且不会重复释放资源或损坏已持久化文件。
3. `Dump` 后创建的新索引实例能够复制并打开此前文件。
4. 两个索引使用不同文件，互不覆盖。
5. 索引析构后不存在仍被占用的文件句柄。
6. 此阶段仍不包含 SQLite 依赖。

---

## M3：引入 SQLite 并实现 FTS5 索引

**状态**: ✅ 已完成并通过本地验收（2026-07-17）

**目标**: 用 SQLite 数据库替换 M2 的最小文件后端，完成 FTS5 建表、写入、查询和持久化恢复。

**独立测试**: 直接构造 `SQLiteFTSIndex`，追加多条文本，执行 MATCH 查询；随后 Dump 并重开，再次执行相同查询并得到相同结果。

### M3.1 SQLite 依赖与封装

SQLite 固定使用官方 `third_party/sqlite` submodule：版本 3.53.3，commit
`92a6c5c3636faa021ecc3be5403a00f50f65eda7`，以 shallow gitlink 引入并从
源码生成 amalgamation 后静态链接。

- [x] F004-T301 为 `sqlite_fts` 扩展引入 SQLite 依赖，限定依赖只对该扩展及其测试可见
- [x] F004-T302 在配置或启动测试中验证 SQLite 编译时启用了 FTS5
- [x] F004-T303 实现扩展内部 SQLite RAII 封装，覆盖 database、statement 和错误信息
- [x] F004-T304 使用参数绑定传递用户文本和查询字符串，禁止拼接用户数据

### M3.2 元数据与建表

- [x] F004-T305 校验索引只绑定单个、非空的 `STRING` 属性
- [x] F004-T306 校验索引名称并生成隔离的 FTS5 表名 `neug_fts_{name}`
- [x] F004-T307 解析并校验 `tokenizer`、`prefix`、`detail`、`rank` 和 `candidate_batch_size`
- [x] F004-T308 在首次 Open 时创建 contentless FTS5 虚拟表
- [x] F004-T309 在恢复 Open 时验证数据库和目标 FTS5 表存在，不重复初始化数据

### M3.3 写入与查询

- [x] F004-T310 实现 `AppendImpl(index_id, value)`，以 `index_id` 作为 SQLite rowid 追加完整字符串
- [x] F004-T311 拒绝 NULL 和非字符串 Value，并返回参数错误
- [x] F004-T312 实现 ranked search，将完整 `query_string` 绑定到 FTS5 MATCH 查询，并使用 topk 限制候选数量
- [x] F004-T313 按 BM25 rank 升序读取 rowid 和 score，并转换为 ranked 候选集合
- [x] F004-T314 保持历史 rowid，不在 SQLite 层覆盖或删除旧版本
- [x] F004-T315 在 `SQLiteFTSIndex` 的 ranked search 包装层完成候选 ID 的有效版本过滤，同时保持 vid 与 score 的对应关系

### M3.4 SQLite 生命周期替换

- [x] F004-T316 用 SQLite 数据库文件替换 M2 的最小文件后端
- [x] F004-T317 `Dump` 前完成 statement finalize、事务提交和数据库 flush，并在 Dump 资源收尾阶段释放 SQLite 连接
- [x] F004-T318 `Dump` 前确保数据库处于可复制的一致状态，不遗漏 WAL 内容
- [x] F004-T319 从 checkpoint 重开 SQLite 文件后继续支持追加和查询
- [x] F004-T320 清理 M2 仅用于测试的文件 payload 实现，但保留其生命周期测试语义

### M3.5 测试

- [x] F004-T321 添加 FTS5 可用性测试
- [x] F004-T322 添加单词、短语、前缀和无匹配结果测试
- [x] F004-T323 添加特殊字符与 SQLite MATCH 语法错误测试
- [x] F004-T324 添加 Upsert 新版本后旧 rowid 被上层过滤的测试
- [x] F004-T325 添加 Delete 后候选被上层过滤的测试
- [x] F004-T326 添加 SQLite 数据库 Dump/重开/继续追加测试
- [x] F004-T327 将 M1 stub 候选结果替换为 SQLite FTS5 的真实匹配结果，并保持用户侧 Cypher 不变
- [x] F004-T328 添加完整端到端测试：建表、插入数据、创建索引、全文查询、关闭数据库、重开数据库、再次查询

**验收条件**:

1. FTS5 表使用 contentless 模式，rowid 等于 `index_id`。
2. 写入文本和 MATCH 查询均使用 SQLite 参数绑定。
3. ranked search 过滤无效历史版本，并保持有效 vid 与 BM25 score 一一对应。
4. 数据库重开前后查询结果一致，并且重开后仍可追加。
5. M1、M2、M3 的测试全部通过。

---

## M4：错误处理、文档与回归

**目标**: 收敛接口行为，验证扩展不会影响现有索引和非 FTS 查询。

**独立测试**: 只加载 `sqlite_fts` 扩展运行现有索引测试和新增测试，确认无行为回归。

- [ ] F004-T401 统一 SQLite 错误到 NeuG `Status` 的转换，保留 SQLite 原始错误信息
- [ ] F004-T402 确保 prepare、bind、step、finalize、open 和 close 失败均有明确上下文
- [ ] F004-T403 补充扩展 README，包括构建、加载、建索引和查询示例
- [ ] F004-T404 记录 `unicode61` 不等同于中文分词，并说明首版中文限制
- [ ] F004-T405 运行 `sqlite_fts_extension_test`
- [ ] F004-T406 运行现有 storage index 测试，确认索引框架无回归
- [ ] F004-T407 运行格式检查和完整相关测试集

---

## 执行顺序与阶段门禁

```text
M0 扩展骨架
  ↓ smoke test 通过
M1 MATCH + 评分 + ORDER BY + LIMIT → 单一融合索引算子
  ↓ 融合计划与端到端测试通过
M2 Open/Dump + 最小文件持久化
  ↓ 生命周期与重开测试通过
M3 SQLite FTS5
  ↓ 索引、持久化、端到端测试通过
M4 回归与文档
```

- 不允许跨过阶段门禁一次性实现全部功能。
- 每个阶段应形成一个可单独审查的提交。
- 如果某阶段暴露核心框架缺口，先在本目录记录问题、复现方式和最小修改建议；未获得确认前不得修改核心文件。

## 暂不纳入首版

- 中文自定义 tokenizer。
- 多属性全文索引。
- NULL 属性索引。
- TP 模式下创建索引。
- 基于事务 accessor 的 in-filtering。
- 稳定分页和复杂 rank 配置。
- 后台 vacuum、历史版本物理回收和索引压缩。
