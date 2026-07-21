#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "neug/storages/index/storage_index.h"
#include "sqlite_fts_sqlite.h"

namespace neug::sqlite_fts_ext {

struct SQLiteFTSQueryParams final : IndexQueryParams {
  std::vector<vid_t> scalar_filter;
  bool use_scalar_filter{false};
  std::string query_string;
  uint32_t topk{10};
};

struct SQLiteFTSRankedResult {
  vid_t vid;
  double score;
};

class SQLiteFTSDumpContainer final : public IDataContainer {
 public:
  SQLiteFTSDumpContainer(SQLiteDatabase* database, std::string runtime_path);

  ContainerType GetContainerType() const override {
    return ContainerType::kFileSharedMMap;
  }
  void Resize(size_t) override {}
  std::string GetPath() const override { return runtime_path_; }
  void Open(const std::string&) override {}
  void Sync() override;
  void Dump(const std::string& new_path) override;
  bool IsDirty() override { return true; }
  std::unique_ptr<IDataContainer> Fork(Checkpoint&, MemoryLevel) override;

 private:
  SQLiteDatabase* database_;
  std::string runtime_path_;
};

class SQLiteFTSIndex final : public StorageIndex {
 public:
  ~SQLiteFTSIndex() override;

  void Open(Checkpoint& ckp, const ModuleDescriptor& descriptor,
            MemoryLevel level) override;
  void Open(Checkpoint& ckp, const CheckpointManifest& manifest,
            const ModuleDescriptor& descriptor, MemoryLevel level) override;
  void Dump(Checkpoint& ckp, CheckpointManifest& manifest,
            const std::string& key) override;
  void Detach(Checkpoint& ckp, MemoryLevel level) override;
  std::unique_ptr<Module> Clone() const override;

  Status BeginBuild() override;
  Status FinishBuild() override;

  result<std::vector<SQLiteFTSRankedResult>> RankedSearch(
      const SQLiteFTSQueryParams& params);
  static bool HasFTS5() { return SQLiteDatabase::HasFTS5(); }
  static std::string SQLiteVersion() { return SQLiteDatabase::Version(); }

  static std::string type_name() { return "sqlite_fts_index"; }

 protected:
  result<std::vector<index_id_t>> SearchImpl(
      const IndexQueryParams& params) override;
  Status AppendImpl(index_id_t index_id, const Value& value) override;

 private:
  struct RankedCandidate {
    index_id_t index_id;
    double score;
  };

  void ParseOptions();
  void OpenInternal(Checkpoint& ckp, const CheckpointManifest* manifest,
                    const ModuleDescriptor& descriptor, MemoryLevel level);
  void CreateTable();
  void ValidateExistingTable();
  result<std::vector<RankedCandidate>> QueryCandidates(
      const SQLiteFTSQueryParams& params);

  static constexpr const char* kIndexFilePath = "sqlite_fts_file";
  static constexpr const char* kAccessorRef = "index_id_accessor";

  SQLiteDatabase database_;
  std::string runtime_path_;
  std::string table_name_;
  std::string tokenizer_{"unicode61"};
  std::string prefix_;
  std::string detail_{"full"};
  std::string rank_{"bm25"};
  uint32_t candidate_batch_size_{128};
};

}  // namespace neug::sqlite_fts_ext
