#pragma once

#include <memory>
#include <string>
#include <vector>

#include <zvec/core/interface/index.h>
#include <zvec/core/interface/index_param.h>
#include <zvec/core/interface/vector_source.h>

#include "neug/storages/container/i_container.h"
#include "neug/storages/index/index_id_accessor.h"
#include "neug/storages/index/storage_index.h"
#include "neug/utils/property/vec_column.h"

namespace neug::zvec_ext {

struct HNSWIndexQueryParams final : IndexQueryParams {
  std::vector<vid_t> scalar_filter;
  Value target_value;
  uint32_t topk{10};
  bool fetch_vector{false};
  float radius{0.0f};
  uint32_t ef_search{100};
  uint32_t prefetch_offset{0};
  uint32_t prefetch_lines{0};
};

class HNSWVecSource final : public zvec::core::VectorSource {
 public:
  HNSWVecSource(const ArrayColumn* column, DataTypeId element_type);

  const void* get_vector(uint32_t node_id) const override;

 private:
  using VectorGetter = const void* (*) (const ArrayColumn*, uint32_t);

  template <typename T>
  static const void* GetVector(const ArrayColumn* column, uint32_t node_id) {
    return column->get_row_ptr<T>(node_id);
  }

  const ArrayColumn* column_;
  VectorGetter vector_getter_;
};

class VecIndexIDAccessor final : public IndexIDAccessor {
 public:
  explicit VecIndexIDAccessor(IndexIDAccessor* offset_accessor)
      : offset_accessor_(offset_accessor) {}

  index_id_t GetIndexIDByVID(vid_t vid) const override;
  vid_t GetVIDByIndexID(index_id_t index_id) const override;
  // VecColumn has already assigned the offset before the index is updated.
  index_id_t UpsertVID(vid_t vid) override;
  // Index deletion also invalidates the mapping owned by VecColumn.
  Status DeleteVID(vid_t vid) override;

  // VecColumn owns persistence and lifecycle management for the accessor.
  void Open(Checkpoint&, const ModuleDescriptor&, MemoryLevel) override {}
  void Dump(Checkpoint&, CheckpointManifest&, const std::string&) override {}

  std::unique_ptr<Module> Clone() const override;
  void Detach(Checkpoint&, MemoryLevel) override {}
  std::string ModuleTypeName() const override {
    return "vec_index_id_accessor";
  }

 private:
  IndexIDAccessor* offset_accessor_;
};

class ZVecDumpContainer final : public IDataContainer {
 public:
  ZVecDumpContainer(zvec::core_interface::Index* index,
                    std::string runtime_path);

  ContainerType GetContainerType() const override {
    return ContainerType::kFileSharedMMap;
  }
  void Resize(size_t) override {}
  std::string GetPath() const override { return runtime_path_; }
  void Open(const std::string&) override {}
  void Sync() override;
  void Dump(const std::string& new_path) override;
  bool IsDirty() override;
  std::unique_ptr<IDataContainer> Fork(Checkpoint&, MemoryLevel) override;

 private:
  zvec::core_interface::Index* zvec_index_;
  std::string runtime_path_;
};

class HNSWIndex final : public StorageIndex {
 public:
  HNSWIndex() = default;

  ~HNSWIndex() override;

  void Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
            MemoryLevel level) override;
  void Dump(Checkpoint& ckp, CheckpointManifest& meta,
            const std::string& key) override;
  Status Rebind(const IndexBindContext& context) override;
  void Detach(Checkpoint& ckp, MemoryLevel level) override;
  std::unique_ptr<Module> Clone() const override;
  static std::string type_name() { return "hnsw_index"; }

 protected:
  result<std::vector<index_id_t>> SearchImpl(
      const IndexQueryParams& params) override;
  Status AppendImpl(index_id_t index_id, const Value& value) override;

 private:
  void ParseOptions();
  zvec::core_interface::DataType ResolveDataType() const;

  std::shared_ptr<zvec::core_interface::Index> zvec_index_;
  std::unique_ptr<HNSWVecSource> vec_source_;
  std::string zvec_runtime_path_;
  int dimension_{0};
  int m_{50};
  int ef_construction_{500};
  zvec::core_interface::MetricType metric_{
      zvec::core_interface::MetricType::kL2sq};
};

}  // namespace neug::zvec_ext
