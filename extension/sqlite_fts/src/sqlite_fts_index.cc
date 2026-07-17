#include "sqlite_fts_index.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include "neug/storages/checkpoint.h"
#include "neug/storages/checkpoint_manifest.h"
#include "neug/storages/index/index_id_accessor.h"
#include "neug/storages/module/module_factory.h"
#include "neug/utils/exception/exception.h"
#include "neug/utils/io/file/file_utils.h"

namespace neug::sqlite_fts_ext {
namespace {

bool IsBlank(const std::string& value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  });
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

std::string ValidateTokenizer(const std::string& tokenizer) {
  static const std::unordered_set<std::string> kSupported = {
      "unicode61", "ascii", "porter", "porter unicode61", "trigram"};
  if (!kSupported.contains(tokenizer)) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "SQLiteFTSIndex tokenizer must be unicode61, ascii, porter, "
        "porter unicode61, or trigram");
  }
  return tokenizer;
}

std::string ValidatePrefix(const std::string& prefix) {
  if (prefix.empty()) {
    return {};
  }
  std::istringstream input(prefix);
  std::string token;
  std::string normalized;
  while (input >> token) {
    if (!std::all_of(token.begin(), token.end(), [](unsigned char character) {
          return std::isdigit(character) != 0;
        })) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "SQLiteFTSIndex prefix must contain positive integers");
    }
    unsigned long value = 0;
    try {
      value = std::stoul(token);
    } catch (const std::exception&) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "SQLiteFTSIndex prefix contains an invalid integer");
    }
    if (value == 0 || value > 999) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "SQLiteFTSIndex prefix values must be between 1 and 999");
    }
    if (!normalized.empty()) {
      normalized += ' ';
    }
    normalized += std::to_string(value);
  }
  if (normalized.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "SQLiteFTSIndex prefix must contain at least one integer");
  }
  return normalized;
}

}  // namespace

SQLiteFTSDumpContainer::SQLiteFTSDumpContainer(SQLiteDatabase* database,
                                               std::string runtime_path)
    : database_(database), runtime_path_(std::move(runtime_path)) {}

void SQLiteFTSDumpContainer::Sync() {
  if (!database_ || !database_->IsOpen()) {
    THROW_RUNTIME_ERROR("SQLiteFTSDumpContainer: database is not open");
  }
  database_->Flush();
  database_->Close();
}

void SQLiteFTSDumpContainer::Dump(const std::string& new_path) {
  if (database_ && database_->IsOpen()) {
    Sync();
  }
  std::filesystem::rename(runtime_path_, new_path);
  runtime_path_ = new_path;
}

std::unique_ptr<IDataContainer> SQLiteFTSDumpContainer::Fork(Checkpoint&,
                                                             MemoryLevel) {
  THROW_NOT_SUPPORTED_EXCEPTION("SQLiteFTSDumpContainer does not support Fork");
}

SQLiteFTSIndex::~SQLiteFTSIndex() { database_.Close(); }

void SQLiteFTSIndex::ParseOptions() {
  if (!meta_) {
    THROW_RUNTIME_ERROR("SQLiteFTSIndex metadata is not initialized");
  }
  if (meta_->name.empty() ||
      !std::regex_match(meta_->name, std::regex("[A-Za-z_][A-Za-z0-9_]*"))) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "SQLiteFTSIndex name must be a non-empty SQL-safe identifier");
  }
  if (meta_->schema.property_name.empty() ||
      meta_->schema.property_type.id() != DataTypeId::kVarchar) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "SQLiteFTSIndex requires one non-empty STRING property");
  }

  static const std::unordered_set<std::string> kKnownOptions = {
      "tokenizer", "prefix", "detail", "rank", "candidate_batch_size"};
  for (const auto& [name, value] : meta_->options) {
    if (!kKnownOptions.contains(ToLower(name))) {
      THROW_INVALID_ARGUMENT_EXCEPTION("Unsupported SQLiteFTSIndex option: " +
                                       name);
    }
  }

  if (auto option = meta_->options.find("tokenizer");
      option != meta_->options.end()) {
    tokenizer_ = ValidateTokenizer(option->second);
  }
  if (auto option = meta_->options.find("prefix");
      option != meta_->options.end()) {
    prefix_ = ValidatePrefix(option->second);
  }
  if (auto option = meta_->options.find("detail");
      option != meta_->options.end()) {
    if (option->second != "full" && option->second != "column" &&
        option->second != "none") {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "SQLiteFTSIndex detail must be full, column, or none");
    }
    detail_ = option->second;
  }
  if (auto option = meta_->options.find("rank");
      option != meta_->options.end()) {
    if (option->second != "bm25") {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "SQLiteFTSIndex currently supports only bm25 rank");
    }
    rank_ = option->second;
  }
  if (auto option = meta_->options.find("candidate_batch_size");
      option != meta_->options.end()) {
    try {
      if (option->second.empty() ||
          !std::all_of(option->second.begin(), option->second.end(),
                       [](unsigned char character) {
                         return std::isdigit(character) != 0;
                       })) {
        throw std::invalid_argument("candidate_batch_size");
      }
      auto parsed = std::stoul(option->second);
      if (parsed == 0 || parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::out_of_range("candidate_batch_size");
      }
      candidate_batch_size_ = static_cast<uint32_t>(parsed);
    } catch (const std::exception&) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "SQLiteFTSIndex candidate_batch_size must be a positive integer");
    }
  }
  table_name_ = "neug_fts_" + meta_->name;
}

void SQLiteFTSIndex::CreateTable() {
  std::string sql = "CREATE VIRTUAL TABLE " + table_name_ +
                    " USING fts5(text, content='', tokenize='" + tokenizer_ +
                    "'";
  if (!prefix_.empty()) {
    sql += ", prefix='" + prefix_ + "'";
  }
  sql += ", detail=" + detail_ + ");";
  database_.Execute(sql);
}

void SQLiteFTSIndex::ValidateExistingTable() {
  auto statement = database_.Prepare(
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 "
      "AND sql LIKE '%USING fts5(%';");
  statement.BindText(1, table_name_);
  if (statement.Step() != SQLITE_ROW) {
    THROW_RUNTIME_ERROR("SQLiteFTSIndex persisted FTS5 table is missing: " +
                        table_name_);
  }
}

void SQLiteFTSIndex::Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
                          MemoryLevel level) {
  OpenInternal(ckp, nullptr, descriptor, level);
}

void SQLiteFTSIndex::Open(Checkpoint& ckp, const CheckpointManifest& manifest,
                          const ModuleDescriptor& descriptor,
                          MemoryLevel level) {
  OpenInternal(ckp, &manifest, descriptor, level);
}

void SQLiteFTSIndex::OpenInternal(Checkpoint& ckp,
                                  const CheckpointManifest* manifest,
                                  const ModuleDescriptor& descriptor,
                                  MemoryLevel level) {
  StorageIndex::Open(ckp, descriptor, level);
  ParseOptions();
  if (!HasFTS5()) {
    THROW_RUNTIME_ERROR("SQLite library was built without FTS5 support");
  }

  if (!index_id_accessor_) {
    index_id_accessor_ = std::make_unique<DefaultIndexIDAccessor>();
  }
  ModuleDescriptor accessor_descriptor;
  if (auto accessor_ref = descriptor.get_ref(kAccessorRef)) {
    const auto& resolver = manifest ? *manifest : ckp.GetMeta();
    auto resolved = resolver.module(*accessor_ref);
    if (!resolved) {
      THROW_RUNTIME_ERROR(
          "SQLiteFTSIndex::Open: missing index ID accessor descriptor");
    }
    accessor_descriptor = std::move(*resolved);
  }
  index_id_accessor_->Open(ckp, accessor_descriptor, level);

  auto runtime_uuid = ckp.CreateRuntimeObject();
  runtime_path_ = ckp.runtime_dir() + "/" + runtime_uuid;
  auto index_path = descriptor.get_path(kIndexFilePath);
  bool has_existing = index_path && !index_path->empty() &&
                      std::filesystem::exists(*index_path);
  try {
    if (has_existing) {
      file_utils::copy_file(*index_path, runtime_path_, true);
    }
    database_.Open(runtime_path_);
    if (has_existing) {
      ValidateExistingTable();
    } else {
      CreateTable();
    }
  } catch (...) {
    database_.Close();
    std::error_code error;
    std::filesystem::remove(runtime_path_, error);
    runtime_path_.clear();
    throw;
  }
}

void SQLiteFTSIndex::Dump(Checkpoint& ckp, CheckpointManifest& manifest,
                          const std::string& key) {
  if (key.empty()) {
    THROW_RUNTIME_ERROR("SQLiteFTSIndex::Dump: module key must not be empty");
  }
  if (!database_.IsOpen()) {
    THROW_RUNTIME_ERROR("SQLiteFTSIndex::Dump: index is not open");
  }

  StorageIndex::Dump(ckp, manifest, key);
  const auto accessor_key = "sqlite_fts_accessor_" + meta_->name;
  index_id_accessor_->Dump(ckp, manifest, accessor_key);
  manifest.mutable_modules().at(accessor_key).mark_as_referenced_module();
  manifest.mutable_modules().at(key).set_ref(kAccessorRef, accessor_key);

  SQLiteFTSDumpContainer container(&database_, runtime_path_);
  auto persisted_path = ckp.Commit(container);
  manifest.mutable_modules().at(key).set_path(kIndexFilePath, persisted_path);

  auto runtime_uuid = ckp.CreateRuntimeObject();
  runtime_path_ = ckp.runtime_dir() + "/" + runtime_uuid;
  file_utils::copy_file(persisted_path, runtime_path_, true);
  database_.Open(runtime_path_);
  ValidateExistingTable();
}

void SQLiteFTSIndex::Detach(Checkpoint&, MemoryLevel) {
  THROW_NOT_SUPPORTED_EXCEPTION("SQLiteFTSIndex does not support Detach");
}

std::unique_ptr<Module> SQLiteFTSIndex::Clone() const {
  THROW_NOT_SUPPORTED_EXCEPTION("SQLiteFTSIndex does not support Clone");
}

result<std::vector<SQLiteFTSIndex::RankedCandidate>>
SQLiteFTSIndex::QueryCandidates(const SQLiteFTSQueryParams& params) {
  if (params.topk == 0) {
    RETURN_INVALID_ARGUMENT_ERROR("SQLITE_FTS topk must be positive");
  }
  if (!database_.IsOpen()) {
    RETURN_INVALID_ARGUMENT_ERROR("SQLITE_FTS index is not open");
  }
  if (params.query_string.empty() || IsBlank(params.query_string)) {
    return std::vector<RankedCandidate>{};
  }

  try {
    auto statement = database_.Prepare(
        "SELECT rowid, " + rank_ + "(" + table_name_ + ") AS score FROM " +
        table_name_ + " WHERE " + table_name_ +
        " MATCH ?1 ORDER BY score ASC LIMIT ?2;");
    statement.BindText(1, params.query_string);
    auto fetch_limit =
        params.use_scalar_filter
            ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            : static_cast<uint64_t>(params.topk) +
                  static_cast<uint64_t>(candidate_batch_size_);
    statement.BindInt64(
        2, static_cast<int64_t>(std::min<uint64_t>(
               fetch_limit,
               static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))));

    std::vector<RankedCandidate> candidates;
    while (statement.Step() == SQLITE_ROW) {
      auto rowid = statement.ColumnInt64(0);
      if (rowid < 0 || static_cast<uint64_t>(rowid) >
                           std::numeric_limits<index_id_t>::max()) {
        continue;
      }
      candidates.push_back(RankedCandidate{static_cast<index_id_t>(rowid),
                                           statement.ColumnDouble(1)});
    }
    return candidates;
  } catch (const std::exception& error) {
    RETURN_STATUS_ERROR(
        StatusCode::ERR_INVALID_ARGUMENT,
        "SQLITE_FTS query failed: " + std::string(error.what()));
  }
}

result<std::vector<SQLiteFTSRankedResult>> SQLiteFTSIndex::RankedSearch(
    const SQLiteFTSQueryParams& params) {
  if (!index_id_accessor_) {
    RETURN_INVALID_ARGUMENT_ERROR(
        "SQLITE_FTS index ID accessor is not initialized");
  }
  auto candidates = QueryCandidates(params);
  if (!candidates) {
    return tl::unexpected(candidates.error());
  }
  std::unordered_set<index_id_t> allowed;
  if (params.use_scalar_filter) {
    allowed.reserve(params.scalar_filter.size());
    for (auto vid : params.scalar_filter) {
      auto index_id = index_id_accessor_->GetIndexIDByVID(vid);
      if (index_id != INVALID_INDEX_ID) {
        allowed.insert(index_id);
      }
    }
  }
  std::vector<SQLiteFTSRankedResult> results;
  results.reserve(params.topk);
  for (const auto& candidate : *candidates) {
    if (params.use_scalar_filter && !allowed.contains(candidate.index_id)) {
      continue;
    }
    auto vid = index_id_accessor_->GetVIDByIndexID(candidate.index_id);
    if (vid == INVALID_VID) {
      continue;
    }
    results.push_back(SQLiteFTSRankedResult{vid, candidate.score});
    if (results.size() == params.topk) {
      break;
    }
  }
  return results;
}

result<std::vector<index_id_t>> SQLiteFTSIndex::SearchImpl(
    const IndexQueryParams& params) {
  const auto* fts_params = dynamic_cast<const SQLiteFTSQueryParams*>(&params);
  if (!fts_params) {
    RETURN_INVALID_ARGUMENT_ERROR(
        "SQLiteFTSIndex::Search requires SQLiteFTSQueryParams");
  }
  auto candidates = QueryCandidates(*fts_params);
  if (!candidates) {
    return tl::unexpected(candidates.error());
  }
  std::vector<index_id_t> results;
  results.reserve(fts_params->topk);
  for (const auto& candidate : *candidates) {
    if (index_id_accessor_->GetVIDByIndexID(candidate.index_id) ==
        INVALID_VID) {
      continue;
    }
    results.push_back(candidate.index_id);
    if (results.size() == fts_params->topk) {
      break;
    }
  }
  return results;
}

Status SQLiteFTSIndex::AppendImpl(index_id_t index_id, const Value& value) {
  if (value.IsNull() || value.type().id() != DataTypeId::kVarchar) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "SQLITE_FTS values must be non-null STRING values");
  }
  if (!database_.IsOpen()) {
    return Status::RuntimeError("SQLiteFTSIndex must be open before append");
  }
  try {
    auto statement = database_.Prepare("INSERT INTO " + table_name_ +
                                       "(rowid, text) VALUES (?1, ?2);");
    statement.BindInt64(1, index_id);
    statement.BindText(2, value.GetValue<std::string>());
    statement.Step();
    return Status::OK();
  } catch (const std::exception& error) {
    return Status::RuntimeError("SQLiteFTSIndex append failed: " +
                                std::string(error.what()));
  }
}

NEUG_REGISTER_MODULE(SQLiteFTSIndex);

}  // namespace neug::sqlite_fts_ext
