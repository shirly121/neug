# SQLite FTS Index 设计

**Status**: Draft  
**Step**: 1 - `StorageIndex` class

## StorageIndex

`StorageIndex` 是所有存储索引的基类，统一管理索引生命周期、DocID 映射、数据更新和查询流程。

```cpp
class StorageIndex : public Module {
 public:
  StorageIndex() = default;
  ~StorageIndex() override = default;

  // 索引初始化，CREATE INDEX 时由框架统一调用
  virtual Status Init(std::string name,
                      std::unique_ptr<IndexMeta> meta) = 0;

  // 清理索引内部结构，DROP INDEX 时由框架统一调用
  virtual void Clear();

  // --- Data operations ---

  // 查询接口，输入查询参数，返回符合查询条件的目标点集
  // 内部调用具体索引实现的 SearchImpl
  result<std::vector<vid_t>> Search(const IndexQueryParams& params);

  // Upsert 单点的索引数据，内部自动判断是 Insert 还是 Update
  // 内部调用具体索引实现的 AppendImpl 追加索引数据
  Status Upsert(vid_t vid, const execution::Value& new_value);

  // 删除接口
  // 默认实现为标记删除，仅删除 DocIDMap 映射
  Status Delete(vid_t vid);

  // --- Module interface ---

  void Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
            MemoryLevel level) override;
  void Dump(Checkpoint& ckp, CheckpointManifest& meta,
            const std::string& key) override;
  void Detach(Checkpoint& ckp, MemoryLevel level) override;
  std::string ModuleTypeName() const override;

 protected:
  // 具体索引实现
  virtual result<std::vector<doc_id_t>> SearchImpl(
      const IndexQueryParams& params) = 0;
  virtual Status AppendImpl(doc_id_t doc_id,
                            const execution::Value& value) = 0;

 protected:
  std::unique_ptr<IndexMeta> meta_;
  std::unique_ptr<DocIDMap> doc_id_map_;
};
```

### 职责划分

- `StorageIndex` 负责 `vid_t` 与 `doc_id_t` 的版本映射。
- `Upsert` 为每次新增或更新分配新的 `doc_id_t`，再调用 `AppendImpl`。
- `Delete` 只删除 `DocIDMap` 中的有效映射，不要求立即清理具体索引中的历史数据。
- `SearchImpl` 返回候选 `doc_id_t`，`Search` 通过 `DocIDMap` 过滤无效版本并转换为 `vid_t`。
- `Init` 和 `Clear` 分别对应 `CREATE INDEX` 和 `DROP INDEX`。
- `Open`、`Dump` 和 `Detach` 负责 checkpoint 持久化与 COW 生命周期。

## SQLiteFTSIndex

`SQLiteFTSIndex` 使用 SQLite FTS5 存储全文索引，不创建 NeuG 点表或边表。

```cpp
class SQLiteFTSIndex : public StorageIndex {
 public:
  SQLiteFTSIndex() = default;
  ~SQLiteFTSIndex() override = default;

  // 索引初始化，CREATE INDEX 时由框架统一调用
  Status Init(std::string name,
              std::unique_ptr<IndexMeta> meta) override {
    // 1. 打开该索引对应的 SQLite 数据库
    // 2. 读取meta信息并配置各类参数
    // 3. 创建 SQLite FTS5 虚拟表
  }

  // 清理索引内部结构，DROP INDEX 时由框架统一调用
  void Clear() override {
    // 1. 删除 SQLite FTS5 表
    // 2. 关闭 SQLite 数据库连接
    // 3. 清理该索引对应的 SQLite 数据文件
  }

 protected:
  Status AppendImpl(doc_id_t doc_id,
                    const execution::Value& value) override {
    // value 为新增或更新后的 string 文本
    // 使用 doc_id 作为 SQLite FTS5 表的 rowid
    // tokenizer / stemmer 由 SQLite FTS5 配置完成
    // 每次 Upsert 只追加新版本，不覆盖旧 doc_id
  }

  result<std::vector<doc_id_t>> SearchImpl(
      const IndexQueryParams& params) override {
    // 1. 从 params 中取得全文查询字符串
    // 2. 执行 SQLite FTS5 MATCH 查询
    // 3. 读取匹配记录的 rowid，转换为 doc_id_t
    // 4. 返回候选 doc_id 集合
    // 5. StorageIndex::Search 统一过滤无效版本并转换为 vid_t
  }

 private:
  std::string db_path_;
  std::string table_name_;
  std::string tokenizer_;
  std::unique_ptr<SQLiteDatabase> database_;
};
```

## 接口实现

### Init 操作

```cpp
Status SQLiteFTSIndex::Init(std::string name,
                            std::unique_ptr<IndexMeta> meta);
```

`Init` 在 `CREATE INDEX` 阶段调用，负责创建 SQLite 实例和 FTS5 表。

1. **打开 SQLite**
    - 确定 SQLite 数据库路径
    - 使用读写模式打开 SQLite 数据库，并保存数据库连接
    - SQLite 打开失败时返回错误

2. **读取索引元数据**
    - 全文索引复用现有的 `IndexMeta`，例如：
    ```cpp
    auto meta = std::make_unique<IndexMeta>();
    meta->type = "sqlite_fts5";
    meta->schema.label_id = label_id;
    meta->schema.property_name = "content";
    meta->schema.property_type = DataType::STRING();
    ```
    - `type` 用于确认索引实现为 `sqlite_fts5`
    - `schema.label_id`、`schema.property_name` 和 `schema.property_type` 用于定位并
      校验被索引的属性
    - `IndexMeta` 中的 `name`、`schema`、`options` 等其他元数据也可用于校验索引
      名称、绑定关系和配置是否合法

3. **读取配置信息**
    - FTS5 配置统一保存在 `IndexMeta::options` 中，例如：
    ```ini
    tokenizer=unicode61
    prefix=2 3 4
    detail=full
    rank=bm25
    candidate_batch_size=128
    ```
    - `tokenizer`、`prefix` 和 `detail` 用于配置 SQLite FTS5 虚拟表
    - `rank` 用于配置查询结果的相关性排序方式
    - `candidate_batch_size` 用于控制每批从 SQLite 读取的候选结果数量
    - 校验各配置项是否合法；配置非法时关闭 SQLite 连接并返回错误

4. **校验索引属性**
    - 第一版仅支持绑定单个非空（非 `NULL`）的 `STRING` 属性
    - 绑定多个属性、非 `STRING` 属性或可为空属性时返回参数错误
    - 根据其他 `IndexMeta` 元数据校验标签是否存在、属性是否属于该标签、声明的
      `property_type` 是否与图 Schema 中的实际类型一致，以及索引名称是否有效

5. **创建 SQLite FTS5 表**
   - 在 SQLite 中创建 FTS5 虚拟表 `neug_fts_{name}`
   - 使用 contentless 模式，不保存原始文本
   - 将 `IndexMeta::options` 中适用于建表的 `tokenizer`、`prefix` 和 `detail`
     转换为 SQLite FTS5 参数
    ```sql
    CREATE VIRTUAL TABLE neug_fts_name USING fts5(
        text,
        content='',
        tokenize='unicode61',
        prefix='2 3 4',
        detail=full
    );
    ```
    - SQLite FTS5 表创建失败时关闭 SQLite 连接并返回错误

6. **中文分词说明**
    - SQLite 自带的 `unicode61` tokenizer 仅提供 Unicode 字符处理能力，不等同于
      中文分词器
    - 中文场景需要配置自定义 FTS5 tokenizer，或者在写入 SQLite 前由应用侧完成
      分词
    - TODO: 详细调研中文分词方案，包括自定义 FTS5 tokenizer 的实现与注册方式、
      应用侧分词的接入位置、写入与查询使用相同分词规则的保证方式，以及不同方案
      对索引大小、查询质量和性能的影响

### AppendImpl 操作

```cpp
Status SQLiteFTSIndex::AppendImpl(doc_id_t doc_id,
                                  const execution::Value& value);
```

`AppendImpl` 在 `StorageIndex::Upsert` 分配新的 `doc_id` 后调用，负责将
`doc_id` 和完整的字符串 `value` 直接传递给 SQLite FTS5 表。`value` 一定是
字符串类型，`AppendImpl` 不对字符串做拆分或预处理。每次调用只追加一个新
版本，不覆盖已有的 `doc_id`。

1. **准备插入语句**
   - 使用 `doc_id` 作为 SQLite FTS5 表的 `rowid`
   - 使用参数绑定传递 `doc_id` 和 `value`，避免直接拼接用户数据
    ```sql
    INSERT INTO neug_fts_name(rowid, text) VALUES (doc_id, Value);
    ```

2. **绑定参数**
   - 将 `doc_id` 绑定到 `rowid`
   - 从 NeuG 的统一 `Value` 中取得完整字符串，并作为 SQLite `TEXT` 参数绑定
     到 `text`
   - TODO: 调研从 NeuG 统一 `Value` 中取得字符串并转换为 SQLite 可接收的
     `TEXT` 参数的具体接口

3. **执行插入**
   - 将绑定后的语句直接交给 SQLite 执行
   - 插入成功时返回成功状态
   - SQLite prepare、参数绑定或执行失败时，返回包含 SQLite 错误信息的失败状态

### SearchImpl 操作

```cpp
result<std::vector<doc_id_t>> SQLiteFTSIndex::SearchImpl(
    const IndexQueryParams& params);
```

`SearchImpl` 负责从 `params` 中取得完整的全文查询字符串，并将查询直接传递给
SQLite FTS5 表。SQLite 完成分词和全文匹配后，`SearchImpl` 将匹配记录对应的
`rowid` 转换为候选 `doc_id_t` 并返回。候选结果是否仍为有效版本，由
`StorageIndex::Search` 统一过滤。

1. **取得查询字符串**
   - 将 `params` 转换为 SQLite FTS 索引对应的查询参数类型
   - 从查询参数中取得完整的 `query_string`
   - 查询字符串不在 NeuG 内部做拆分或预处理，直接使用 SQLite FTS5 的
     `MATCH` 语法解释
   - 查询参数类型不匹配时返回参数错误

2. **准备查询语句**
   - 在 SQLite FTS5 表的 `text` 列上执行全文匹配
   - 使用参数绑定传递 `query_string`，避免直接拼接用户输入
    ```sql
    SELECT rowid FROM neug_fts_name WHERE text MATCH '$query_string' ORDER BY rank;
    ```

3. **执行查询并转换结果**
   - 将绑定后的查询语句交给 SQLite 执行
   - 遍历所有匹配记录，读取记录对应的 `rowid`
   - 将每个 `rowid` 转换为 `doc_id_t`，并追加到候选结果集合
   - 没有匹配记录时返回空集合
   - SQLite prepare、参数绑定、执行或结果读取失败时，返回包含 SQLite 错误信息
     的失败状态

## MVCC

### 可见性模型

FTS5 内部保存同一条逻辑数据的所有历史版本。数据发生新增或修改时，索引框架
为新版本分配新的 `rowid`，`SQLiteFTSIndex` 仅将新版本追加到 FTS5 表中，不
覆盖或删除旧版本。

事务可见性由索引框架为当前事务提供的 accessor 统一判断，`SQLiteFTSIndex` 和
`SearchImpl` 不需要感知事务版本，也不负责过滤失效的 `rowid`。`SearchImpl`
只返回 SQLite FTS5 匹配到的候选 `rowid`，索引框架再通过当前事务的 accessor
完成可见性过滤，并将有效的 `rowid` 映射回对应的图数据。

例如，数据列中的文本从 `old` 修改为 `new` 后，FTS5 中同时保存两个版本：

```text
(rowid=10, text='old')
(rowid=27, text='new')
```

- 对于发生本次修改的当前事务，其 accessor 将 `rowid=10` 标记为
  `invalid`，仅 `rowid=27` 有效
- 对于修改前已经开始的旧事务，其 accessor 中 `rowid=10` 仍然有效，因此仍可
  读取旧版本
- 两个事务可以复用同一个包含所有版本的 FTS5 索引，并通过各自的 accessor
  得到符合自身事务快照的查询结果

### In-filtering（TODO）

当用户查询中包含 `LIMIT` 时，全文索引需要限制最终返回的有效结果数量。但
MVCC 可见性过滤由索引框架统一执行，发生在 `SearchImpl` 返回候选结果之后，
属于 post-filtering。如果直接在 SQLite FTS5 查询中使用用户指定的 `LIMIT`，
部分候选 `rowid` 可能在框架层被 accessor 判定为不可见，导致最终结果数量小于
用户指定的 `LIMIT`。

第一版考虑使用 over-fetching：在 FTS5 检索时额外获取一部分候选数据，再由
索引框架执行 MVCC 可见性过滤。额外获取的数量可结合
`candidate_batch_size` 配置控制。该方案能够降低结果不足的概率，但不能保证
过滤后一定得到 `LIMIT` 条有效结果，并且可能产生额外的 SQLite 查询、数据读取
和可见性判断开销。

更正确的做法是在按 `rank` 遍历 FTS5 查询结果的过程中，使用当前事务的
accessor 实时检查每个 `rowid` 的可见性。每发现一条可见记录就加入最终结果，
直到得到 `LIMIT` 条有效记录或 FTS5 结果耗尽。这样可以同时保证相关性顺序和
`LIMIT` 语义。

TODO: 详细调研 in-filtering 的可行性，包括：

- `SearchImpl` 是否可以安全访问当前事务的 accessor，或者由索引框架向遍历过程
  提供可见性判断回调
- SQLite FTS5 结果能否按 `rank` 分批、连续遍历，并在达到有效结果数量后提前停止
- 分批查询的分页方式，以及如何避免候选结果重复、遗漏或排序不稳定
- in-filtering 对现有索引接口分层、事务快照一致性和查询性能的影响
- over-fetching 作为首版方案时，`candidate_batch_size`、用户 `LIMIT` 与实际候选
  数量之间的计算规则
