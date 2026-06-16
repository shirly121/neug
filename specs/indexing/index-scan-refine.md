# 优化 HNSW Index Scan 的相关实现

当前已经实现了 hnsw index scan 的部分功能，现在需要进一步改进

## zvec extension

### 增加 vec_distance_l2 函数

### 增加 HNSWIndexScanRule

在 zvec extension 模块中添加可插拔的优化器规则，将向量查询优化为 HNSWIndexScan 算子，

实现可插拔的优化器规则:

```

```