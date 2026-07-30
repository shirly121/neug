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

VecColumnBuffer::VecColumnBuffer(std::unique_ptr<ArrayColumn> buffer)
    : buffer_(std::move(buffer)) {
  if (!buffer_) {
    THROW_INVALID_ARGUMENT_EXCEPTION("VecColumnBuffer requires an ArrayColumn");
  }
}

size_t VecColumnBuffer::size() const {
  std::shared_lock lock(mutex_);
  return buffer_->size();
}

void VecColumnBuffer::resize(size_t size) {
  std::unique_lock lock(mutex_);
  buffer_->resize(size);
}

void VecColumnBuffer::resize(size_t size, const Value& default_value) {
  std::unique_lock lock(mutex_);
  buffer_->resize(size, default_value);
}

Value VecColumnBuffer::get_any(size_t index) const {
  std::shared_lock lock(mutex_);
  return buffer_->get_any(index);
}

void VecColumnBuffer::set_any(size_t index, const Value& value,
                              bool insert_safe) {
  std::shared_lock lock(mutex_);
  buffer_->set_any(index, value, insert_safe);
}

const void* VecColumnBuffer::get_buffer_ptr() const {
  std::shared_lock lock(mutex_);
  return buffer_->get_buffer_ptr();
}

void VecColumnBuffer::ingest(uint32_t index, OutArchive& arc) {
  std::shared_lock lock(mutex_);
  buffer_->ingest(index, arc);
}

void VecColumnBuffer::Open(Checkpoint& ckp, const ModuleDescriptor& desc,
                           MemoryLevel level) {
  std::unique_lock lock(mutex_);
  buffer_->Open(ckp, desc, level);
}

void VecColumnBuffer::Open(Checkpoint& ckp, const CheckpointManifest& manifest,
                           const ModuleDescriptor& desc, MemoryLevel level) {
  std::unique_lock lock(mutex_);
  buffer_->Open(ckp, manifest, desc, level);
}

void VecColumnBuffer::Dump(Checkpoint& ckp, CheckpointManifest& manifest,
                           const std::string& key) {
  std::shared_lock lock(mutex_);
  buffer_->Dump(ckp, manifest, key);
}

VecColumn::VecColumn()
    : offset_accessor_(std::make_unique<DefaultIndexIDAccessor>()),
      buffer_(
          std::make_shared<VecColumnBuffer>(std::make_unique<ArrayColumn>())) {}

VecColumn::VecColumn(std::unique_ptr<ArrayColumn> buffer,
                     std::unique_ptr<IndexIDAccessor> accessor, size_t vid_size)
    : offset_accessor_(std::move(accessor)),
      buffer_(std::make_shared<VecColumnBuffer>(std::move(buffer))) {
  if (!offset_accessor_) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "VecColumn requires a buffer and an offset accessor");
  }
  if (vid_size > buffer_->size()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "VecColumn vid size exceeds the underlying buffer size");
  }
  for (size_t vid = 0; vid < vid_size; ++vid) {
    offset_accessor_->UpsertVID(static_cast<vid_t>(vid));
  }
}

void VecColumn::resize(size_t size) {
  auto offset_size = offset_accessor_->size();
  auto growth = offset_size + offset_size / 4;
  auto new_size = offset_size < 4096 || size >= growth ? size : growth;
  if (new_size > buffer_->size())
    buffer_->resize(new_size);
}

void VecColumn::resize(size_t size, const Value& default_value) {
  auto offset_size = offset_accessor_->size();
  auto growth = offset_size + offset_size / 4;
  auto new_size = offset_size < 4096 || size >= growth ? size : growth;
  if (new_size > buffer_->size())
    buffer_->resize(new_size, default_value);
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

std::unique_ptr<Module> VecColumn::Clone() const {
  auto accessor = offset_accessor_->Clone();
  auto cloned = std::make_unique<VecColumn>();
  cloned->buffer_ = buffer_;
  cloned->offset_accessor_.reset(
      static_cast<IndexIDAccessor*>(accessor.release()));
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
  desc.set_ref(kBufferRef, buffer_key);
  desc.set_ref(kAccessorRef, accessor_key);
  meta.set_module(key, std::move(desc));
}

NEUG_REGISTER_MODULE(VecColumn);
}  // namespace neug
