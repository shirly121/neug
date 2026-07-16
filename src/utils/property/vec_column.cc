#include "neug/utils/property/vec_column.h"

#include <limits>
#include <string>

#include "neug/storages/checkpoint_manifest.h"
#include "neug/storages/module/module_factory.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace {
constexpr const char* kBufferRef = "buffer";
constexpr const char* kAccessorRef = "offset_accessor";
}  // namespace

VecColumn::VecColumn()
    : offset_accessor_(std::make_unique<DefaultIndexIDAccessor>()),
      buffer_(std::make_shared<ArrayColumn>()) {}

VecColumn::VecColumn(std::shared_ptr<ArrayColumn> buffer,
                     std::unique_ptr<IndexIDAccessor> accessor)
    : offset_accessor_(std::move(accessor)), buffer_(std::move(buffer)) {
  if (!buffer_ || !offset_accessor_) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "VecColumn requires a buffer and an offset accessor");
  }
  vid_size_ = buffer_->size();
  for (size_t vid = 0; vid < vid_size_; ++vid) {
    offset_accessor_->UpsertVID(static_cast<vid_t>(vid));
  }
  buffer_->resize(EstimateOffsetCapacity(vid_size_));
}

size_t VecColumn::EstimateOffsetCapacity(size_t vid_size) {
  if (vid_size == 0)
    return 0;
  if (vid_size > std::numeric_limits<size_t>::max() / 2)
    return vid_size;
  return vid_size * 2;
}

void VecColumn::resize(size_t size) {
  vid_size_ = size;
  auto capacity = EstimateOffsetCapacity(size);
  if (capacity > buffer_->size())
    buffer_->resize(capacity);
}

void VecColumn::resize(size_t size, const Value& default_value) {
  auto old_size = vid_size_;
  resize(size);
  for (size_t vid = old_size; vid < size; ++vid) {
    set_any(vid, default_value, true);
  }
}

void VecColumn::checkOffset(index_id_t offset, const char* operation) const {
  if (offset >= buffer_->size()) {
    THROW_RUNTIME_ERROR(std::string("VecColumn::") + operation + ": offset " +
                        std::to_string(offset) + " out of range (capacity=" +
                        std::to_string(buffer_->size()) + ")");
  }
}

void VecColumn::set_any(size_t vid, const Value& value, bool insert_safe) {
  if (vid > std::numeric_limits<vid_t>::max()) {
    THROW_INVALID_ARGUMENT_EXCEPTION("VecColumn::set_any: vid out of range");
  }
  auto offset = offset_accessor_->UpsertVID(static_cast<vid_t>(vid));
  checkOffset(offset, "set_any");
  buffer_->set_any(offset, value, insert_safe);
}

Value VecColumn::get_any(size_t vid) const {
  if (vid > std::numeric_limits<vid_t>::max())
    return Value(DataType::SQLNULL);
  auto offset = get_offset(static_cast<vid_t>(vid));
  if (offset == INVALID_INDEX_ID)
    return Value(DataType::SQLNULL);
  checkOffset(offset, "get_any");
  return buffer_->get_any(offset);
}

void VecColumn::ingest(uint32_t vid, OutArchive& arc) {
  auto offset = offset_accessor_->UpsertVID(vid);
  checkOffset(offset, "ingest");
  buffer_->ingest(offset, arc);
}

index_id_t VecColumn::get_offset(vid_t vid) const {
  return offset_accessor_->GetIndexIDByVID(vid);
}

Value VecColumn::get_offset_value(index_id_t offset) const {
  checkOffset(offset, "get_offset_value");
  return buffer_->get_any(offset);
}

std::unique_ptr<ArrayColumn> VecColumn::TakeBuffer() {
  if (!buffer_)
    return nullptr;
  auto clone = buffer_->Clone();
  buffer_.reset();
  auto result =
      std::unique_ptr<ArrayColumn>(static_cast<ArrayColumn*>(clone.release()));
  result->setLogicalSize(vid_size_);
  return result;
}

std::unique_ptr<Module> VecColumn::Clone() const {
  auto accessor = offset_accessor_->Clone();
  auto cloned = std::make_unique<VecColumn>();
  cloned->buffer_ = buffer_;
  cloned->offset_accessor_.reset(
      static_cast<IndexIDAccessor*>(accessor.release()));
  cloned->vid_size_ = vid_size_;
  return cloned;
}

void VecColumn::Detach(Checkpoint& ckp, MemoryLevel level) {
  offset_accessor_->Detach(ckp, level);
}

void VecColumn::Open(Checkpoint& ckp, const ModuleDescriptor& desc,
                     MemoryLevel level) {
  openInternal(ckp, nullptr, desc, level);
}

void VecColumn::Open(Checkpoint& ckp, const CheckpointManifest& manifest,
                     const ModuleDescriptor& desc, MemoryLevel level) {
  openInternal(ckp, &manifest, desc, level);
}

void VecColumn::openInternal(Checkpoint& ckp,
                             const CheckpointManifest* manifest,
                             const ModuleDescriptor& desc, MemoryLevel level) {
  const auto* resolver = manifest ? manifest : &ckp.GetMeta();
  auto buffer_ref = desc.get_ref(kBufferRef);
  auto accessor_ref = desc.get_ref(kAccessorRef);
  if (buffer_ref && accessor_ref) {
    auto buffer_desc = resolver->module(*buffer_ref);
    auto accessor_desc = resolver->module(*accessor_ref);
    if (!buffer_desc || !accessor_desc) {
      THROW_RUNTIME_ERROR("VecColumn::Open: missing referenced module");
    }
    buffer_->Open(ckp, *resolver, *buffer_desc, level);
    offset_accessor_->Open(ckp, *accessor_desc, level);
    vid_size_ = std::stoull(desc.get("vid_size").value_or("0"));
    return;
  }
  if (!desc.module_type.empty()) {
    THROW_RUNTIME_ERROR("VecColumn::Open: missing buffer/accessor refs");
  }
  buffer_->Open(ckp, ModuleDescriptor{}, level);
  offset_accessor_->Open(ckp, ModuleDescriptor{}, level);
}

void VecColumn::Dump(Checkpoint& ckp, CheckpointManifest& meta,
                     const std::string& key) {
  if (key.empty())
    THROW_RUNTIME_ERROR("VecColumn::Dump: empty module key");
  auto buffer_key = key + "/buffer";
  auto accessor_key = key + "/offset_accessor";
  buffer_->Dump(ckp, meta, buffer_key);
  offset_accessor_->Dump(ckp, meta, accessor_key);
  meta.mutable_modules().at(buffer_key).mark_as_referenced_module();
  meta.mutable_modules().at(accessor_key).mark_as_referenced_module();
  ModuleDescriptor desc;
  desc.module_type = ModuleTypeName();
  desc.set("vid_size", std::to_string(vid_size_));
  desc.set_ref(kBufferRef, buffer_key);
  desc.set_ref(kAccessorRef, accessor_key);
  meta.set_module(key, std::move(desc));
}

NEUG_REGISTER_MODULE(VecColumn);
}  // namespace neug
