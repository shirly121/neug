#pragma once

#include <memory>

#include "neug/storages/index/index_id_accessor.h"
#include "neug/utils/property/array_column.h"

namespace neug {

class VecColumn : public ColumnBase {
 public:
  VecColumn();
  VecColumn(std::shared_ptr<ArrayColumn> buffer,
            std::unique_ptr<IndexIDAccessor> accessor);

  void Open(Checkpoint&, const ModuleDescriptor&, MemoryLevel) override;
  void Open(Checkpoint&, const CheckpointManifest&, const ModuleDescriptor&,
            MemoryLevel) override;
  void Dump(Checkpoint&, CheckpointManifest&, const std::string&) override;
  size_t size() const override { return vid_size_; }
  void resize(size_t size) override;
  void resize(size_t size, const Value& default_value) override;
  DataTypeId type() const override { return DataTypeId::kArray; }
  void set_any(size_t vid, const Value& value, bool insert_safe) override;
  Value get_any(size_t vid) const override;
  void ingest(uint32_t vid, OutArchive& arc) override;

  index_id_t get_offset(vid_t vid) const;
  Value get_offset_value(index_id_t offset) const;
  IndexIDAccessor* get_offset_accessor() const {
    return offset_accessor_.get();
  }
  const ArrayColumn* get_buffer() const { return buffer_.get(); }
  std::unique_ptr<ArrayColumn> TakeBuffer();
  std::unique_ptr<Module> Clone() const override;
  void Detach(Checkpoint&, MemoryLevel) override;
  std::string ModuleTypeName() const override { return type_name(); }
  static std::string type_name() { return "column<vector>"; }

 private:
  static size_t EstimateOffsetCapacity(size_t vid_size);
  void openInternal(Checkpoint&, const CheckpointManifest*,
                    const ModuleDescriptor&, MemoryLevel);
  void checkOffset(index_id_t offset, const char* operation) const;

  std::unique_ptr<IndexIDAccessor> offset_accessor_;
  std::shared_ptr<ArrayColumn> buffer_;
  size_t vid_size_{0};
};

class VecRefColumn : public RefColumnBase {
 public:
  explicit VecRefColumn(const VecColumn& column) : column_(column) {}
  Value get_any(size_t index) const override { return column_.get_any(index); }
  DataTypeId type() const override { return DataTypeId::kArray; }
  ColType col_type() const override { return ColType::kInternal; }
  index_id_t get_offset(vid_t vid) const { return column_.get_offset(vid); }
  Value get_offset_value(index_id_t offset) const {
    return column_.get_offset_value(offset);
  }

 private:
  const VecColumn& column_;
};

}  // namespace neug
