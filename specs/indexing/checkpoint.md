# 重新实现 Index 持久化

## 基类 Index 实现

```c++
class Index : Module {
public:
    // descriptor 格式：
    // index_meta: "{}"
    // next_doc_id: 1000
    // doc_id_buffer: "/data/checkpoint/snapshot/xx"
    // 实现 DocIDMap 和 IndexMeta 反序列化
    void Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
                        MemoryLevel level) {
        // 从 descriptor 获取 index_meta 对应的 json 字符串
        index_meta = IndexMeta::readJson(...);
        
        doc_id_map = std::make_unique<DocIDMap>();
        doc_id_map->Open(ckp, descriptor, level);
    }

    ModuleDescriptor Dump(Checkpoint& ckp) {
        ModuleDescriptor desc;
        // 不需要设置 module_type，由子类设置具体类型
        // 持久化 index_meta
        desc.set("index_meta", index_meta.dumpJson());
        // 持久化 doc_id_map
        auto doc_desc = doc_id_map.Dump(ckp);
        desc.set("next_doc_id", doc_desc.get("next_doc_id"));
        desc.set_path("doc_id_buffer", doc_desc.get("doc_id_buffer"));
        return desc;
    }

    
private:
    std::unique_ptr<IndexMeta> index_meta;
    std::unique_ptr<DocIDMap> doc_id_map;
}
```

## DocIDMap 实现

```c++
void DocIDMap::Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
                    MemoryLevel level) {
  auto next_doc_id_str = descriptor.get("next_doc_id");
  if (next_doc_id_str.has_value()) {
    next_doc_id_.store(
        static_cast<doc_id_t>(std::stoull(next_doc_id_str.value())),
        std::memory_order_relaxed);
  }

  // manifest key 修改为 doc_id_buffer，不存在设置为空
  auto path = descriptor.get_path("doc_id_buffer").value_or("");
  // 直接调用 ckp.OpenFile，不需要判断 path 是否存在
  buffer_ = ckp.OpenFile(*path, level);
}

ModuleDescriptor DocIDMap::Dump(Checkpoint& ckp) {
  ModuleDescriptor desc;
  desc.module_type = "doc_id_map";
  desc.set("next_doc_id",
           std::to_string(next_doc_id_.load(std::memory_order_relaxed)));
  desc.set_path("doc_id_buffer", ckp.Commit(*buffer_));
  return desc;
}
```

## HNSWIndex 实现

修改 Open 实现：
```c++
class HNSWIndex : Index {
    void Open(Checkpoint& ckp, const ModuleDescriptor& desc,
                        MemoryLevel level) {
        // 调用基类的 Open 函数构造 index_meta 和 doc_id_map
        
        // 初始化 index_runtime_path_
        // 创建 index_runtime_path_ = ckp.runtime_dir() + "/" + ckp.CreateRuntimeObject()
        // 从 desc 中获取 index_path
        index_path = desc.get_path("index_buffer");
        // 如果 index_runtime_path == index_path，则什么都不做
        // 否则：
        //      如果 index_path 不为空且文件存在，则将 index_path 文件内容拷贝到 index_runtime_path_
        //      else，重新创建 index_runtime_path_ 文件 （存在情况下删除）

        // 构建 IndexFactory 参数
        auto param = HNSWIndexParamBuilder()
                .WithMetricType(...) // 从 IndexMeta::options 中获取
                .WithDataType(...) // 从 IndexMeta::BindSchema 中获取
                .WithDimension(kDimension) // 从 IndexMeta::BindSchema 中获取
                .WithIsSparse(false) 
                .Build();
        zvec_index_ = ...
        
        // 构建 StorageOptions
        StorageOptions opts;
        opts.type = StorageOptions::StorageType::kMMAP;
        opts.create_new = false;
        opts.read_only = false;
        // 仅支持 MMAP_SHARED
        opts.copy_on_write = false;

        int ret = zvec_index_->Open(index_runtime_path_, opts);
        ...
    }
private:
    std::shared_ptr<zvec::core_interface::Index> zvec_index_;
    std::string index_runtime_path_;
}
```

修改 Dump 实现：

```c++
// 适配器：将 ZVec 索引文件操作适配为 IDataContainer 接口
// 使得 ZVec 索引文件可以通过 Checkpoint::Commit() 统一提交
class ZVecDumpContainer : IDataContainer {
public:
    ZVecDumpContainer(zvec::core_interface::Index *index, const std::string &index_runtime_path)
        : zvec_index_(index), runtime_path_(index_runtime_path) {}

    // 数据是否发生修改，没有修改直接走 hardlink 快路径
    bool IsDirty() override {
        // 目前 ZVec 无法提供 IsDirty 接口，先无脑返回 true
        return true;
    }

    // 获取索引文件的 runtime_path
    std::string GetPath() override {
        return runtime_path_;
    }

    // 基于 ZVec Flush 操作，将内存数据刷新到 runtime 文件中
    void Sync() override {
        zvec_index_->Flush();
    }

    // 将 runtime_path 重命名为 new_path
    // Commit 会调用此方法
    void Dump(const std::string& new_path) override {
        Sync();
        std::filesystem::rename(runtime_path_, new_path);
    }

private:
    zvec::core_interface::Index *index_;
    std::string runtime_path_;
};

class HNSWIndex : Index {
public:
    ModuleDescriptor HNSWIndex::Dump(Checkpoint& ckp) {
        // 调用基类持久化 index_meta 和 doc_id_map
        auto desc = Index::Dump(ckp);
        // 子类设置具体类型
        desc.module_type = "hnsw_index";
        // 持久化 hnsw index，并设置 index_buffer
        ZVecDumpContainer zvec_container(this);
        desc.set_path("index_buffer", ckp.Commit(zvec_container));    
        return desc;
    }
}
```

## Index Manifest

索引在 Manifest 中结构为：

```
"index_<index_unique_name>": {
    "index_meta": {},
    "next_doc_id": 10000
    "doc_id_buffer": "/data/checkpoint/XXX"
    "index_buffer": "/data/checkpoint/XXX"
}
```

## 注册 Module

在 hnsw_index.cc 通过 `NEUG_REGISTER_MODULE` 注册 HNSWIndex 构造方法

## IndexManager

重构 IndexManager Open/Dump 接口：

```c++
class IndexManager {
public:
    void Open(
        std::shared_ptr<Checkpoint> ckp,
        ModuleBroker& store,
        const CheckpointManifest& meta,
        MemoryLevel memory_level) {
        for ([name, desc] : meta.modules()) {
            if (IndexModule(name)) {
                auto index_module = store.TakeModule<Index>(name);
                indexes_.emplace_back(std::move(index_module));
            }
        }
        ckp_ = std::move(ckp);
        memory_level_ = memory_level;
    }

    bool IndexModule(const std::string &name) {
        return name.startWith("index_");
    }

    void Dump(std::shared_ptr<Checkpoint> ckp, CheckpointManifest& meta) {
        for (auto &index : indexes) {
            auto desc = index->Dump(ckp);
            // 从 index::IndexMeta 获取 index_name
            index_name = ....
            meta.set_module(getKey(index_name), desc);
        }
    }

    std::string getKey(const std::string &index_name) {
        return "index_" + index_name;
    }

private:
    std::unique_ptr<Checkpoint> ckp_;
    MemoryLevel memory_level_;
    std::unordered_map<std::string, std::shared_ptr<Index>> indexes_;
}
```

## Refine DB::Open 和 DB::Dump 接口

目前 Neug DB Open/Dump 只考虑了 PropertyGraph，无法执行 IndexManager 相关功能。

PropertyGraph 增加接口，不改动原先接口：

```c++
class PropertyGraph {
public:
    // 增加 Open 接口
    void Open(
        std::shared_ptr<Checkpoint> ckp,
        ModuleBroker& store,
        const CheckpointManifest& meta,
        MemoryLevel memory_level);

    // 增加 Dump 接口
    // 将 store.dump, ckp->updateMeta, reopen 这些过程放在更上层调用
    void Dump(std::shared_ptr<Checkpoint> ckp, CheckpointManifest& meta);
}
```

在 DB::Open/Dump 入口增加 IndexManager::Open/Dump 流程