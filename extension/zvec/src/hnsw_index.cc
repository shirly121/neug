#include "hnsw_index.h"

#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include <glog/logging.h>
#include <zvec/core/interface/index_factory.h>
#include <roaring.hh>

#include "neug/common/extra_type_info.h"
#include "neug/storages/checkpoint.h"
#include "neug/storages/checkpoint_manifest.h"
#include "neug/storages/module/module_factory.h"
#include "neug/utils/exception/exception.h"
#include "neug/utils/io/file/file_utils.h"

namespace neug::zvec_ext {
namespace {
constexpr const char* kIndexBufferPath = "index_buffer";

struct DenseValueBuffer {
  std::variant<std::vector<float>, std::vector<double>> values;

  const void* data() const {
    return std::visit(
        [](const auto& buffer) -> const void* { return buffer.data(); },
        values);
  }
};

DenseValueBuffer ConvertDenseValue(const Value& value, DataTypeId element_type,
                                   size_t dimension) {
  if (value.IsNull() || value.type().id() != DataTypeId::kArray) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "HNSW vector value must be a non-null ARRAY");
  }
  const auto& children = ArrayValue::GetChildren(value);
  if (children.size() != dimension) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "HNSW vector dimension mismatch: expected " +
        std::to_string(dimension) + ", got " + std::to_string(children.size()));
  }

  DenseValueBuffer result;
  if (element_type == DataTypeId::kFloat) {
    std::vector<float> buffer;
    buffer.reserve(dimension);
    for (const auto& child : children) {
      if (child.IsNull() || child.type().id() != DataTypeId::kFloat) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "HNSW FLOAT vector contains an invalid element");
      }
      buffer.push_back(child.GetValue<float>());
    }
    result.values = std::move(buffer);
    return result;
  }
  if (element_type == DataTypeId::kDouble) {
    std::vector<double> buffer;
    buffer.reserve(dimension);
    for (const auto& child : children) {
      if (child.IsNull() || child.type().id() != DataTypeId::kDouble) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "HNSW DOUBLE vector contains an invalid element");
      }
      buffer.push_back(child.GetValue<double>());
    }
    result.values = std::move(buffer);
    return result;
  }
  THROW_INVALID_ARGUMENT_EXCEPTION(
      "HNSW supports only FLOAT or DOUBLE vector elements");
}

int ParsePositive(const common::case_insensitive_map_t<std::string>& options,
                  const std::string& key, int default_value) {
  auto iter = options.find(key);
  if (iter == options.end())
    return default_value;
  try {
    auto value = std::stoi(iter->second);
    if (value <= 0)
      throw std::invalid_argument("not positive");
    return value;
  } catch (const std::exception&) {
    THROW_INVALID_ARGUMENT_EXCEPTION("HNSW option '" + key +
                                     "' must be a positive integer");
  }
}
}  // namespace

HNSWVecSource::HNSWVecSource(const ArrayColumn* column, DataTypeId element_type)
    : column_(column), vector_getter_(nullptr) {
  if (element_type == DataTypeId::kFloat) {
    vector_getter_ = &GetVector<float>;
  } else if (element_type == DataTypeId::kDouble) {
    vector_getter_ = &GetVector<double>;
  }
}

const void* HNSWVecSource::get_vector(uint32_t node_id) const {
  return vector_getter_(column_, node_id);
}

ZVecDumpContainer::ZVecDumpContainer(zvec::core_interface::Index* index,
                                     std::string runtime_path)
    : zvec_index_(index), runtime_path_(std::move(runtime_path)) {}

void ZVecDumpContainer::Sync() {
  if (zvec_index_ && zvec_index_->Flush() != 0) {
    THROW_RUNTIME_ERROR("[zvec] Failed to flush HNSW index");
  }
}

void ZVecDumpContainer::Dump(const std::string& new_path) {
  Sync();
  std::filesystem::rename(runtime_path_, new_path);
  runtime_path_ = new_path;
}

bool ZVecDumpContainer::IsDirty() {
  return zvec_index_ && zvec_index_->IsDirty();
}

std::unique_ptr<IDataContainer> ZVecDumpContainer::Fork(Checkpoint&,
                                                        MemoryLevel) {
  THROW_NOT_SUPPORTED_EXCEPTION("ZVecDumpContainer does not support Fork");
}

HNSWIndex::~HNSWIndex() {
  if (zvec_index_)
    zvec_index_->Close();
}

void HNSWIndex::ParseOptions() {
  if (!meta_)
    THROW_RUNTIME_ERROR("HNSWIndex metadata is not initialized");
  dimension_ = ParsePositive(meta_->options, "dimension", dimension_);
  m_ = ParsePositive(meta_->options, "m", m_);
  ef_construction_ =
      ParsePositive(meta_->options, "ef_construction", ef_construction_);

  auto metric = meta_->options.find("metric");
  if (metric == meta_->options.end() || metric->second == "l2" ||
      metric->second == "l2sq") {
    metric_ = zvec::core_interface::MetricType::kL2sq;
  } else if (metric->second == "cosine") {
    metric_ = zvec::core_interface::MetricType::kCosine;
  } else if (metric->second == "inner_product" || metric->second == "ip") {
    metric_ = zvec::core_interface::MetricType::kInnerProduct;
  } else {
    THROW_INVALID_ARGUMENT_EXCEPTION("Unsupported HNSW metric: " +
                                     metric->second);
  }

  if (meta_->schema.property_type.id() != DataTypeId::kArray) {
    THROW_INVALID_ARGUMENT_EXCEPTION("HNSWIndex requires an ARRAY property");
  }
  dimension_ =
      static_cast<int>(ArrayType::GetNumElements(meta_->schema.property_type));
}

zvec::core_interface::DataType HNSWIndex::ResolveDataType() const {
  auto child = ArrayType::GetChildType(meta_->schema.property_type).id();
  if (child == DataTypeId::kFloat) {
    return zvec::core_interface::DataType::DT_FP32;
  }
  if (child == DataTypeId::kDouble) {
    return zvec::core_interface::DataType::DT_FP64;
  }
  THROW_INVALID_ARGUMENT_EXCEPTION(
      "HNSWIndex supports only FLOAT or DOUBLE arrays");
}

void HNSWIndex::Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
                     MemoryLevel level) {
  StorageIndex::Open(ckp, descriptor, level);
  ParseOptions();

  zvec::core_interface::HNSWIndexParam param(metric_, dimension_, m_,
                                             ef_construction_);
  param.data_type = ResolveDataType();
  param.use_external_vector = false;
  zvec_index_ = zvec::core_interface::IndexFactory::CreateAndInitIndex(param);
  if (!zvec_index_) {
    THROW_RUNTIME_ERROR("[zvec] Failed to create HNSW index");
  }

  auto runtime_uuid = ckp.CreateRuntimeObject();
  zvec_runtime_path_ = ckp.runtime_dir() + "/" + runtime_uuid;
  auto index_path = descriptor.get_path(kIndexBufferPath);
  bool has_existing = index_path && !index_path->empty() &&
                      std::filesystem::exists(*index_path);
  if (has_existing) {
    file_utils::copy_file(*index_path, zvec_runtime_path_, true);
  }

  zvec::core_interface::StorageOptions options;
  options.type = zvec::core_interface::StorageOptions::StorageType::kMMAP;
  options.create_new = !has_existing;
  options.read_only = false;
  auto ret = zvec_index_->Open(zvec_runtime_path_, options);
  if (ret != 0) {
    THROW_RUNTIME_ERROR("[zvec] Failed to open HNSW index at " +
                        zvec_runtime_path_ +
                        ", error code: " + std::to_string(ret));
  }
  LOG(INFO) << "[zvec] Opened HNSW index at " << zvec_runtime_path_;
}

void HNSWIndex::Dump(Checkpoint& ckp, CheckpointManifest& manifest,
                     const std::string& key) {
  if (key.empty())
    THROW_RUNTIME_ERROR("HNSWIndex::Dump: module key must not be empty");
  if (!zvec_index_)
    THROW_RUNTIME_ERROR("HNSWIndex::Dump: index is not open");

  StorageIndex::Dump(ckp, manifest, key);
  if (zvec_index_->GetDocCount() == 0) {
    return;
  }

  ZVecDumpContainer container(zvec_index_.get(), zvec_runtime_path_);
  auto persisted_path = ckp.Commit(container);
  zvec_runtime_path_ = persisted_path;

  auto descriptor = manifest.mutable_modules().find(key);
  if (descriptor == manifest.mutable_modules().end()) {
    THROW_RUNTIME_ERROR(
        "HNSWIndex::Dump: StorageIndex did not write module descriptor for '" +
        key + "'");
  }
  descriptor->second.set_path(kIndexBufferPath, std::move(persisted_path));
}

Status HNSWIndex::Rebind(const IndexBindContext& context) {
  auto* column = dynamic_cast<const VecColumn*>(context.column);
  if (!column) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "HNSWIndex requires a VecColumn binding");
  }
  if (!meta_ || meta_->schema.property_type.id() != DataTypeId::kArray) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "HNSWIndex metadata does not describe an ARRAY");
  }
  auto element_type = ArrayType::GetChildType(meta_->schema.property_type).id();
  if (element_type != DataTypeId::kFloat &&
      element_type != DataTypeId::kDouble) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "HNSWIndex supports only FLOAT or DOUBLE arrays");
  }
  auto* offset_accessor = column->get_offset_accessor();
  if (!offset_accessor) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "HNSWIndex requires a bound VecColumn offset accessor");
  }
  auto* array_column = column->get_buffer();
  if (!array_column) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "HNSWIndex requires a bound ArrayColumn buffer");
  }
  vec_source_ = std::make_unique<HNSWVecSource>(array_column, element_type);
  auto* vec_accessor =
      dynamic_cast<VecIndexIDAccessor*>(index_id_accessor_.get());
  if (vec_accessor) {
    vec_accessor->Rebind(offset_accessor);
  } else {
    index_id_accessor_ = std::make_unique<VecIndexIDAccessor>(offset_accessor);
  }
  return Status::OK();
}

void HNSWIndex::Detach(Checkpoint&, MemoryLevel) {
  THROW_NOT_SUPPORTED_EXCEPTION("HNSWIndex does not support Detach");
}

std::unique_ptr<Module> HNSWIndex::Clone() const {
  THROW_NOT_SUPPORTED_EXCEPTION("HNSWIndex does not support Clone");
}

result<std::vector<index_id_t>> HNSWIndex::SearchImpl(
    const IndexQueryParams& params) {
  const auto* hnsw_params = dynamic_cast<const HNSWIndexQueryParams*>(&params);
  if (!hnsw_params) {
    RETURN_INVALID_ARGUMENT_ERROR(
        "HNSWIndex::Search requires HNSWIndexQueryParams");
  }
  if (!zvec_index_ || !vec_source_) {
    RETURN_INVALID_ARGUMENT_ERROR(
        "HNSWIndex must be open and bound before search");
  }
  if (hnsw_params->topk == 0 || hnsw_params->ef_search == 0) {
    RETURN_INVALID_ARGUMENT_ERROR(
        "HNSW search topk and ef_search must be positive");
  }

  DenseValueBuffer target;
  try {
    target = ConvertDenseValue(
        hnsw_params->target_value,
        ArrayType::GetChildType(meta_->schema.property_type).id(), dimension_);
  } catch (const std::exception& e) { RETURN_INVALID_ARGUMENT_ERROR(e.what()); }

  auto query_param = std::make_shared<zvec::core_interface::HNSWQueryParam>();
  query_param->topk = hnsw_params->topk;
  query_param->fetch_vector = hnsw_params->fetch_vector;
  query_param->radius = hnsw_params->radius;
  query_param->ef_search = hnsw_params->ef_search;
  query_param->prefetch_offset = hnsw_params->prefetch_offset;
  query_param->prefetch_lines = hnsw_params->prefetch_lines;

  if (hnsw_params->use_scalar_filter) {
    auto allowed = std::make_shared<roaring::Roaring>();
    for (auto vid : hnsw_params->scalar_filter) {
      auto index_id = index_id_accessor_->GetIndexIDByVID(vid);
      if (index_id != INVALID_INDEX_ID)
        allowed->add(index_id);
    }
    allowed->runOptimize();
    query_param->filter = std::make_shared<zvec::core_interface::IndexFilter>();
    query_param->filter->set([allowed](uint64_t key) {
      return key > std::numeric_limits<uint32_t>::max() ||
             !allowed->contains(static_cast<uint32_t>(key));
    });
  }

  zvec::core_interface::VectorData query;
  query.vector = zvec::core_interface::DenseVector{target.data()};
  zvec::core_interface::SearchResult search_result;
  auto ret = zvec_index_->SearchWithSource(query, query_param, *vec_source_,
                                           &search_result);
  if (ret != 0) {
    RETURN_ERROR(Status::RuntimeError(
        "ZVec HNSW search failed with error code " + std::to_string(ret)));
  }

  std::vector<index_id_t> result;
  result.reserve(search_result.doc_list_.size());
  for (const auto& document : search_result.doc_list_) {
    if (document.key() <= std::numeric_limits<index_id_t>::max()) {
      result.push_back(static_cast<index_id_t>(document.key()));
    }
  }
  return result;
}

Status HNSWIndex::AppendImpl(index_id_t index_id, const Value& value) {
  if (!zvec_index_ || !vec_source_) {
    return Status::RuntimeError(
        "HNSWIndex must be open and bound before append");
  }

  DenseValueBuffer dense;
  try {
    dense = ConvertDenseValue(
        value, ArrayType::GetChildType(meta_->schema.property_type).id(),
        dimension_);
  } catch (const std::exception& e) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT, e.what());
  }

  zvec::core_interface::VectorData vector;
  vector.vector = zvec::core_interface::DenseVector{dense.data()};
  auto ret = zvec_index_->AddWithSource(vector, index_id, *vec_source_);
  if (ret != 0) {
    return Status::RuntimeError("ZVec HNSW append failed with error code " +
                                std::to_string(ret));
  }
  return Status::OK();
}

NEUG_REGISTER_MODULE(HNSWIndex);
}  // namespace neug::zvec_ext
