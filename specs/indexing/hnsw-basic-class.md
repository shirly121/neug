## IndexMeta

```c++
struct IndexMeta {
    // 索引 unique_name，例如 'vec_hnsw_index'
    std::string name;
    // 索引类型名称，例如 'HNSW'
    std::string type;
    // 索引绑定的 label, properties 信息
    // 可以支持 vertex or triplet edge type
    IndexBindSchema schema;
    // 索引类型相关的配置参数，
    // WITH (metric='cosine', ef_search=...)
    case_insensitive_map_t<Value> &options;

};

struct IndexBindSchema {
    LabelEntry label;
    // 一个索引可能绑定多个属性，虽然目前 NeuG 支持的只有一列属性
    std::vector<std::string> property_names;
    std::vector<neug::DataType> property_types;
}

enum EntryType {
    VERTEX = 1,
    EDGE = 2;
}

// denote a vertex or triplet edge type
struct LabelEntry {
    EntryType type;
    std::string label_name;
    label_t label_id;
    std::string src_label_name;
    label_t src_label_id;
    std::string dst_label_name;
    label_t dst_label_id; 
};
```

## IndexQueryParams

基础类：

```c++
struct IndexQueryParams {
  virtual ~IndexQueryParams() = 0;
};
```

HNSW 特殊实现:
```c++
struct HNSWIndexQueryParams : IndexQueryParams {
  const void* query_vector;
  int dimension;
  int topk;
};
```

## IndexFilterParams

```c++
struct IndexFilterParams {
  neug::Context pattern_filter;
};
```

## Index 接口

```c++
class Index : public Module {
 public:
  // MUST 定义这个构造函数，不能漏掉这些参数
  explicit Index(
        const std::string &name, // 索引 unique_name
        const IndexMeta &meta, // 索引元数据
		const IStorageInterface &transaction); // 用于 ZVec 访问 NeuG 向量存储数据 
  
  ~Index() override = default;

  // --- Module interface ---
  void Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
            MemoryLevel level) override = 0;
  ModuleDescriptor Dump(Checkpoint& ckp) override = 0;
  std::string ModuleTypeName() const override { return "index"; }

  // --- Data operations ---

  /**
   * @brief Search for nearest neighbors.
   * @param params Query parameters (vector, dimension, topk).
   * @param filter_params Optional MVCC filter.
   * @param results Output: vid_t results after doc_id -> vid translation.
   */
  virtual Status Search(const IndexQueryParams& params,
                        const IndexFilterParams& filter_params,
                        std::vector<vid_t>& results) = 0;

  /**
   * @brief Append a vector for the given vertex id.
   *
   * Internally allocates a doc_id via DocIDMap and inserts into the
   * underlying index structure.
   *
   * @param vid The vertex id.
   * @param vector_data Pointer to raw vector data (float array).
   * @param dimension Number of dimensions in the vector.
   */
//   virtual Status Append(vid_t vid, const void* vector_data,
//    
//                      int dimension) = 0;
    // Index 是通用接口，不能直接定义 HNSW 特例的 vector_data, dimension 参数
    Status Append(
        vid_t vid, // 当前新增点
        const std::vector<Property> &values // 当前新增点的属性
    );

  /**
   * @brief Mark a vertex as deleted in the index.
   *
   * Append-Only design: only marks the entry, does not physically remove.
   */
  virtual Status Delete(vid_t vid) = 0;

  // --- COW support ---

  /**
   * @brief Shallow copy: shared_ptr shares doc_id_map and raw index.
   */
  virtual std::shared_ptr<Index> Fork() const = 0;

  /**
   * @brief Write-time deep copy of DocIDMap when shared.
   */
  virtual void LazyFork() = 0;

  // --- Metadata ---
  const IndexMeta& GetMeta() const { return meta_; }
  
  // 这个接口定义了有什么用？没有特殊用处的话直接去除
//   uint32_t GetDocCount() const {
//     return doc_id_map_ ? doc_id_map_->size() : 0;
//   }

 protected:
  IndexMeta meta_;
  std::shared_ptr<DocIDMap> doc_id_map_;
};
```

