#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "neug/main/connection.h"
#include "neug/main/neug_db.h"
#include "neug/storages/checkpoint.h"
#include "neug/storages/index/index_id_accessor.h"
#include "sqlite_fts_index.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace neug::sqlite_fts_ext {
namespace {

class TemporaryDatabaseDirectory {
 public:
  TemporaryDatabaseDirectory() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("neug_sqlite_fts_smoke_" + std::to_string(suffix));
  }

  ~TemporaryDatabaseDirectory() { std::filesystem::remove_all(path_); }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::filesystem::path GetExecutablePath() {
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  return std::filesystem::canonical(buffer.c_str());
#else
  return std::filesystem::read_symlink("/proc/self/exe");
#endif
}

std::string FindBuildRoot() {
  auto directory = GetExecutablePath().parent_path();
  const auto extension_path = std::filesystem::path(
      "extension/sqlite_fts/libsqlite_fts.neug_extension");
  for (int i = 0; i < 8; ++i) {
    if (std::filesystem::exists(directory / extension_path)) {
      return directory.string();
    }
    if (directory == directory.parent_path()) {
      break;
    }
    directory = directory.parent_path();
  }
  return "";
}

TEST(SQLiteFTSExtensionTest, LoadSucceeds) {
  const auto build_root = FindBuildRoot();
  ASSERT_FALSE(build_root.empty());
  ASSERT_EQ(setenv("NEUG_EXTENSION_HOME_PYENV", build_root.c_str(), 1), 0);

  TemporaryDatabaseDirectory database_directory;
  neug::NeugDB database;
  ASSERT_TRUE(database.Open(database_directory.path()));
  auto connection = database.Connect();
  ASSERT_NE(connection, nullptr);

  auto load = connection->Query("LOAD sqlite_fts;");
  ASSERT_TRUE(load.has_value()) << load.error().ToString();
}

std::unique_ptr<SQLiteFTSIndex> MakeOpenedIndex(Checkpoint& checkpoint) {
  auto meta = std::make_unique<IndexMeta>();
  meta->name = "item_text_fts";
  meta->type = "SQLITE_FTS";
  meta->schema.label_id = 0;
  meta->schema.property_name = "text";
  meta->schema.property_type = DataType(DataTypeId::kVarchar);
  auto index = std::make_unique<SQLiteFTSIndex>();
  auto status =
      index->Init(std::move(meta), std::make_unique<DefaultIndexIDAccessor>());
  EXPECT_TRUE(status.ok()) << status.error_message();
  index->Open(checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory);
  return index;
}

std::unique_ptr<SQLiteFTSIndex> MakeUnopenedIndex(
    const std::string& name = "item_text_fts") {
  auto meta = std::make_unique<IndexMeta>();
  meta->name = name;
  meta->type = "SQLITE_FTS";
  meta->schema.label_id = 0;
  meta->schema.property_name = "text";
  meta->schema.property_type = DataType(DataTypeId::kVarchar);
  auto index = std::make_unique<SQLiteFTSIndex>();
  auto status =
      index->Init(std::move(meta), std::make_unique<DefaultIndexIDAccessor>());
  EXPECT_TRUE(status.ok()) << status.error_message();
  return index;
}

SQLiteFTSQueryParams MakeQuery(std::string query, uint32_t topk = 10) {
  SQLiteFTSQueryParams params;
  params.query_string = std::move(query);
  params.topk = topk;
  return params;
}

TEST(SQLiteFTSIndexTest, FTS5IsAvailable) {
  EXPECT_TRUE(SQLiteFTSIndex::HasFTS5());
  EXPECT_EQ(SQLiteFTSIndex::SQLiteVersion(), "3.53.3");
}

TEST(SQLiteFTSIndexTest, RankedSearchSupportsWordsPhrasesAndPrefixes) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);
  auto index = MakeOpenedIndex(*checkpoint);
  auto first = index->Upsert(7, Value::STRING("quick brown fox"));
  ASSERT_TRUE(first.ok()) << first.error_message();
  auto second = index->Upsert(3, Value::STRING("quick blue hare"));
  ASSERT_TRUE(second.ok()) << second.error_message();
  auto third = index->Upsert(9, Value::STRING("slow brown bear"));
  ASSERT_TRUE(third.ok()) << third.error_message();

  const std::vector<std::pair<std::string, std::vector<vid_t>>> cases = {
      {"quick", {7, 3}}, {"\"quick brown\"", {7}},
      {"bro*", {7, 9}},  {"missing", {}},
      {"", {}},          {"   ", {}}};
  for (const auto& [query, expected] : cases) {
    auto params = MakeQuery(query);
    auto results = index->RankedSearch(params);
    ASSERT_TRUE(results.has_value())
        << query << ": " << results.error().ToString();
    ASSERT_EQ(results->size(), expected.size()) << query;
    for (size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(results->at(i).vid, expected[i]) << query;
      EXPECT_LE(results->at(i).score, 0.0);
    }
  }

  auto invalid = MakeQuery("unterminated\"");
  auto invalid_result = index->RankedSearch(invalid);
  EXPECT_FALSE(invalid_result.has_value());
  EXPECT_NE(invalid_result.error().ToString().find("SQLITE_FTS query failed"),
            std::string::npos);
}

TEST(SQLiteFTSIndexTest, BulkBuildCommitsInsertedRows) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);
  auto index = MakeOpenedIndex(*checkpoint);

  ASSERT_TRUE(index->BeginBuild().ok());
  ASSERT_TRUE(index->Upsert(7, Value::STRING("bulk build fox")).ok());
  ASSERT_TRUE(index->Upsert(8, Value::STRING("bulk build hare")).ok());
  ASSERT_TRUE(index->FinishBuild().ok());

  auto results = index->RankedSearch(MakeQuery("bulk"));
  ASSERT_TRUE(results.has_value()) << results.error().ToString();
  ASSERT_EQ(results->size(), 2);
  EXPECT_EQ(results->at(0).vid, 7u);
  EXPECT_EQ(results->at(1).vid, 8u);
}

TEST(SQLiteFTSExtensionTest, FusedTopKQueryReturnsNodesAndScores) {
  const auto build_root = FindBuildRoot();
  ASSERT_FALSE(build_root.empty());
  ASSERT_EQ(setenv("NEUG_EXTENSION_HOME_PYENV", build_root.c_str(), 1), 0);

  TemporaryDatabaseDirectory database_directory;
  NeugDB database;
  ASSERT_TRUE(database.Open(database_directory.path()));
  auto connection = database.Connect();
  ASSERT_NE(connection, nullptr);
  ASSERT_TRUE(connection->Query("LOAD sqlite_fts;").has_value());
  ASSERT_TRUE(connection
                  ->Query("CREATE NODE TABLE Item(id INT64 PRIMARY KEY, "
                          "text STRING);")
                  .has_value());
  ASSERT_TRUE(connection
                  ->Query("CREATE (:Item {id: 1, text: 'search text alpha'}), "
                          "(:Item {id: 2, text: 'search text beta'}), "
                          "(:Item {id: 3, text: 'gamma'});")
                  .has_value());
  auto create_index = connection->Query(
      "CREATE INDEX item_text_fts ON Item USING SQLITE_FTS (text);");
  ASSERT_TRUE(create_index.has_value()) << create_index.error().ToString();

  auto result = connection->Query(
      "MATCH (n:Item) "
      "RETURN n.id, sqlite_fts_bm25(n.text, 'search text') AS score "
      "ORDER BY score ASC LIMIT 2;");
  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  ASSERT_EQ(result->length(), 2);
  ASSERT_EQ(result->response().arrays_size(), 2);
  EXPECT_EQ(result->response().arrays(0).int64_array().values(0), 1);
  EXPECT_EQ(result->response().arrays(0).int64_array().values(1), 2);
  EXPECT_LE(result->response().arrays(1).double_array().values(0), 0.0);
  EXPECT_LE(result->response().arrays(1).double_array().values(1), 0.0);
  auto explain = connection->Query(
      "EXPLAIN MATCH (n:Item) "
      "RETURN n.id, sqlite_fts_bm25(n.text, 'search text') AS score "
      "ORDER BY score ASC LIMIT 2;");
  ASSERT_TRUE(explain.has_value()) << explain.error().ToString();
  const auto plan_text = explain->profile_result_text();
  EXPECT_NE(plan_text.find("IndexScanOpr"), std::string::npos);
  EXPECT_EQ(plan_text.find("OrderByOpr"), std::string::npos);
  EXPECT_EQ(plan_text.find("LimitOpr"), std::string::npos);

  const std::vector<std::string> query_literals = {"''", "'   '"};
  for (const auto& query_literal : query_literals) {
    auto recognized = connection->Query(
        "MATCH (n:Item) RETURN n.id, sqlite_fts_bm25(n.text, " + query_literal +
        ") AS score ORDER BY score ASC LIMIT 2;");
    EXPECT_TRUE(recognized.has_value())
        << query_literal << ": "
        << (recognized.has_value() ? "" : recognized.error().ToString());
  }
  auto invalid_match = connection->Query(
      "MATCH (n:Item) RETURN n.id, "
      "sqlite_fts_bm25(n.text, \"a 'quoted' phrase\") AS score "
      "ORDER BY score ASC LIMIT 2;");
  EXPECT_FALSE(invalid_match.has_value());
}

TEST(SQLiteFTSExtensionTest, ReopenPreservesIndexAndAcceptsNewRows) {
  const auto build_root = FindBuildRoot();
  ASSERT_FALSE(build_root.empty());
  ASSERT_EQ(setenv("NEUG_EXTENSION_HOME_PYENV", build_root.c_str(), 1), 0);
  TemporaryDatabaseDirectory database_directory;

  {
    NeugDB database;
    ASSERT_TRUE(database.Open(database_directory.path()));
    auto connection = database.Connect();
    ASSERT_TRUE(connection->Query("LOAD sqlite_fts;").has_value());
    ASSERT_TRUE(connection
                    ->Query("CREATE NODE TABLE Item(id INT64 PRIMARY KEY, "
                            "text STRING);")
                    .has_value());
    ASSERT_TRUE(
        connection->Query("CREATE (:Item {id: 1, text: 'durable fox'});")
            .has_value());
    ASSERT_TRUE(connection
                    ->Query("CREATE INDEX item_text_fts ON Item USING "
                            "SQLITE_FTS (text);")
                    .has_value());
    connection.reset();
    database.Close();
  }

  {
    NeugDB database;
    ASSERT_TRUE(database.Open(database_directory.path()));
    auto connection = database.Connect();
    ASSERT_TRUE(connection->Query("LOAD sqlite_fts;").has_value());
    auto restored = connection->Query(
        "MATCH (n:Item) RETURN n.id, "
        "sqlite_fts_bm25(n.text, 'durable') AS score "
        "ORDER BY score ASC LIMIT 10;");
    ASSERT_TRUE(restored.has_value()) << restored.error().ToString();
    ASSERT_EQ(restored->length(), 1);
    EXPECT_EQ(restored->response().arrays(0).int64_array().values(0), 1);

    ASSERT_TRUE(
        connection->Query("CREATE (:Item {id: 2, text: 'durable hare'});")
            .has_value());
    auto appended = connection->Query(
        "MATCH (n:Item) RETURN n.id, "
        "sqlite_fts_bm25(n.text, 'durable') AS score "
        "ORDER BY score ASC LIMIT 10;");
    ASSERT_TRUE(appended.has_value()) << appended.error().ToString();
    EXPECT_EQ(appended->length(), 2);
  }
}

TEST(SQLiteFTSExtensionTest, UnsupportedShapesReturnErrors) {
  const auto build_root = FindBuildRoot();
  ASSERT_FALSE(build_root.empty());
  ASSERT_EQ(setenv("NEUG_EXTENSION_HOME_PYENV", build_root.c_str(), 1), 0);

  TemporaryDatabaseDirectory database_directory;
  NeugDB database;
  ASSERT_TRUE(database.Open(database_directory.path()));
  auto connection = database.Connect();
  ASSERT_NE(connection, nullptr);
  ASSERT_TRUE(connection->Query("LOAD sqlite_fts;").has_value());
  ASSERT_TRUE(connection
                  ->Query("CREATE NODE TABLE Item(id INT64 PRIMARY KEY, "
                          "text STRING);")
                  .has_value());
  ASSERT_TRUE(
      connection->Query("CREATE (:Item {id: 1, text: 'alpha'});").has_value());
  ASSERT_TRUE(connection
                  ->Query("CREATE INDEX item_text_fts ON Item USING "
                          "SQLITE_FTS (text);")
                  .has_value());

  const std::vector<std::string> unsupported = {
      "MATCH (n:Item) RETURN sqlite_fts_bm25(n.text, 'alpha');",
      "MATCH (n:Item) RETURN n.id, sqlite_fts_bm25(n.text, 'alpha') AS "
      "score ORDER BY score DESC LIMIT 1;",
      "MATCH (n:Item) RETURN n.id, sqlite_fts_bm25(n.text, 'alpha') AS "
      "score ORDER BY score ASC;",
      "MATCH (n:Item) RETURN n.id, sqlite_fts_bm25(n.text, 'alpha') AS "
      "score LIMIT 1;",
      "MATCH (n:Item) RETURN n.id, sqlite_fts_bm25(n.text, 'alpha') AS "
      "score ORDER BY score ASC, n.id ASC LIMIT 1;"};
  for (const auto& query : unsupported) {
    auto result = connection->Query(query);
    EXPECT_FALSE(result.has_value()) << query;
  }

  auto wrong_type = connection->Query(
      "MATCH (n:Item) RETURN n.id, sqlite_fts_bm25(n.text, 42) AS score "
      "ORDER BY score ASC LIMIT 1;");
  EXPECT_FALSE(wrong_type.has_value());
}

TEST(SQLiteFTSExtensionTest, MissingIndexReturnsError) {
  const auto build_root = FindBuildRoot();
  ASSERT_FALSE(build_root.empty());
  ASSERT_EQ(setenv("NEUG_EXTENSION_HOME_PYENV", build_root.c_str(), 1), 0);

  TemporaryDatabaseDirectory database_directory;
  NeugDB database;
  ASSERT_TRUE(database.Open(database_directory.path()));
  auto connection = database.Connect();
  ASSERT_NE(connection, nullptr);
  ASSERT_TRUE(connection->Query("LOAD sqlite_fts;").has_value());
  ASSERT_TRUE(connection
                  ->Query("CREATE NODE TABLE Item(id INT64 PRIMARY KEY, "
                          "text STRING);")
                  .has_value());
  auto result = connection->Query(
      "MATCH (n:Item) "
      "RETURN n.id, sqlite_fts_bm25(n.text, 'alpha') AS score "
      "ORDER BY score ASC LIMIT 1;");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().ToString().find("SQLITE_FTS index not found"),
            std::string::npos);
}

TEST(SQLiteFTSIndexTest, RejectsInvalidMetadataAndParams) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);
  auto meta = std::make_unique<IndexMeta>();
  meta->schema.property_type = DataType::INT64;
  SQLiteFTSIndex index;
  auto status =
      index.Init(std::move(meta), std::make_unique<DefaultIndexIDAccessor>());
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_ANY_THROW(
      index.Open(*checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory));

  auto valid_index = MakeOpenedIndex(*checkpoint);
  SQLiteFTSQueryParams params;
  params.query_string = "alpha";
  params.topk = 0;
  auto result = valid_index->RankedSearch(params);
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(valid_index->Upsert(1, Value()).ok());
  EXPECT_FALSE(valid_index->Upsert(2, Value::INT64(42)).ok());
}

TEST(SQLiteFTSIndexTest, ValidatesNameAndFTSOptions) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);

  const std::vector<std::pair<std::string, std::pair<std::string, std::string>>>
      invalid_cases = {{"bad-name", {"", ""}},
                       {"valid_name", {"tokenizer", "unknown"}},
                       {"valid_name", {"prefix", "2 bad"}},
                       {"valid_name", {"detail", "invalid"}},
                       {"valid_name", {"rank", "custom"}},
                       {"valid_name", {"candidate_batch_size", "0"}}};
  for (const auto& [name, option] : invalid_cases) {
    auto index = MakeUnopenedIndex(name);
    if (!option.first.empty()) {
      const_cast<IndexMeta&>(index->GetMeta()).options[option.first] =
          option.second;
    }
    EXPECT_ANY_THROW(
        index->Open(*checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory))
        << name << " " << option.first;
  }

  auto valid = MakeUnopenedIndex("configured_fts");
  auto& options = const_cast<IndexMeta&>(valid->GetMeta()).options;
  options["tokenizer"] = "porter unicode61";
  options["prefix"] = "2 3";
  options["detail"] = "full";
  options["rank"] = "bm25";
  options["candidate_batch_size"] = "32";
  EXPECT_NO_THROW(
      valid->Open(*checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory));
}

TEST(SQLiteFTSIndexTest, FiltersSupersededAndDeletedRowsWithScores) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);
  auto index = MakeOpenedIndex(*checkpoint);
  ASSERT_TRUE(index->Upsert(7, Value::STRING("legacy token")).ok());
  ASSERT_TRUE(index->Upsert(8, Value::STRING("legacy token")).ok());
  ASSERT_TRUE(index->Upsert(7, Value::STRING("current token")).ok());

  auto legacy = index->RankedSearch(MakeQuery("legacy"));
  ASSERT_TRUE(legacy.has_value()) << legacy.error().ToString();
  ASSERT_EQ(legacy->size(), 1);
  EXPECT_EQ(legacy->front().vid, 8u);

  ASSERT_TRUE(index->Delete(8).ok());
  auto after_delete = index->RankedSearch(MakeQuery("legacy"));
  ASSERT_TRUE(after_delete.has_value()) << after_delete.error().ToString();
  EXPECT_TRUE(after_delete->empty());

  auto current = index->RankedSearch(MakeQuery("current"));
  ASSERT_TRUE(current.has_value()) << current.error().ToString();
  ASSERT_EQ(current->size(), 1);
  EXPECT_EQ(current->front().vid, 7u);
  EXPECT_LE(current->front().score, 0.0);
}

TEST(SQLiteFTSIndexTest, CopyOnWriteLifecycleIsNotSupported) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);
  SQLiteFTSIndex index;

  EXPECT_ANY_THROW(index.Clone());
  EXPECT_ANY_THROW(index.Detach(*checkpoint, MemoryLevel::kInMemory));
}

TEST(SQLiteFTSIndexTest, EmptyIndexOpenAndDump) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);
  auto index = MakeUnopenedIndex();
  index->Open(*checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory);

  CheckpointManifest manifest;
  index->Dump(*checkpoint, manifest, "index_item_text_fts");
  auto descriptor = manifest.module("index_item_text_fts");
  ASSERT_TRUE(descriptor.has_value());
  auto path = descriptor->get_path("sqlite_fts_file");
  ASSERT_TRUE(path.has_value());
  EXPECT_TRUE(std::filesystem::is_regular_file(*path));
  std::ifstream file(*path, std::ios::binary);
  ASSERT_TRUE(file.is_open());
  char header[16]{};
  file.read(header, sizeof(header));
  EXPECT_EQ(std::string(header, 15), "SQLite format 3");
}

TEST(SQLiteFTSIndexTest, RepeatedDumpIsIdempotent) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);
  auto index = MakeUnopenedIndex();
  index->Open(*checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory);

  CheckpointManifest first_manifest;
  index->Dump(*checkpoint, first_manifest, "index_item_text_fts");
  auto first = first_manifest.module("index_item_text_fts");
  ASSERT_TRUE(first.has_value());

  CheckpointManifest second_manifest;
  EXPECT_NO_THROW(
      index->Dump(*checkpoint, second_manifest, "index_item_text_fts"));
  auto second = second_manifest.module("index_item_text_fts");
  ASSERT_TRUE(second.has_value());
  auto second_path = second->get_path("sqlite_fts_file");
  ASSERT_TRUE(second_path.has_value());
  EXPECT_TRUE(std::filesystem::is_regular_file(*second_path));
}

TEST(SQLiteFTSIndexTest, DumpAndReopenPreservesSearchAndAllowsAppend) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);
  auto index = MakeUnopenedIndex();
  index->Open(*checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory);
  ASSERT_TRUE(index->Upsert(7, Value::STRING("persisted fox")).ok());

  CheckpointManifest manifest;
  index->Dump(*checkpoint, manifest, "index_item_text_fts");
  auto descriptor = manifest.module("index_item_text_fts");
  ASSERT_TRUE(descriptor.has_value());

  SQLiteFTSIndex restored;
  restored.Open(*checkpoint, manifest, *descriptor, MemoryLevel::kInMemory);
  auto before_append = restored.RankedSearch(MakeQuery("persisted"));
  ASSERT_TRUE(before_append.has_value()) << before_append.error().ToString();
  ASSERT_EQ(before_append->size(), 1);
  EXPECT_EQ(before_append->front().vid, 7u);
  ASSERT_TRUE(restored.Upsert(8, Value::STRING("persisted hare")).ok());
  auto after_append = restored.RankedSearch(MakeQuery("persisted"));
  ASSERT_TRUE(after_append.has_value()) << after_append.error().ToString();
  ASSERT_EQ(after_append->size(), 2);
  CheckpointManifest restored_manifest;
  restored.Dump(*checkpoint, restored_manifest, "index_item_text_fts");
  auto restored_descriptor = restored_manifest.module("index_item_text_fts");
  ASSERT_TRUE(restored_descriptor.has_value());
  auto restored_path = restored_descriptor->get_path("sqlite_fts_file");
  ASSERT_TRUE(restored_path.has_value());
  EXPECT_TRUE(std::filesystem::is_regular_file(*restored_path));
}

TEST(SQLiteFTSIndexTest, MissingFileCreatesNewIndex) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);
  auto index = MakeUnopenedIndex();
  index->Open(*checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory);
  CheckpointManifest manifest;
  index->Dump(*checkpoint, manifest, "index_item_text_fts");
  auto descriptor = manifest.module("index_item_text_fts");
  ASSERT_TRUE(descriptor.has_value());
  auto path = descriptor->get_path("sqlite_fts_file");
  ASSERT_TRUE(path.has_value());

  index.reset();
  ASSERT_TRUE(std::filesystem::remove(*path));
  SQLiteFTSIndex missing;
  EXPECT_NO_THROW(
      missing.Open(*checkpoint, manifest, *descriptor, MemoryLevel::kInMemory));
}

TEST(SQLiteFTSIndexTest, MultipleIndexesUseIsolatedFiles) {
  TemporaryDatabaseDirectory directory;
  auto checkpoint = Checkpoint::Open(directory.path().string(), 0);
  auto first = MakeUnopenedIndex("first_fts");
  auto second = MakeUnopenedIndex("second_fts");
  first->Open(*checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory);
  second->Open(*checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory);

  CheckpointManifest manifest;
  first->Dump(*checkpoint, manifest, "index_first_fts");
  second->Dump(*checkpoint, manifest, "index_second_fts");
  auto first_descriptor = manifest.module("index_first_fts");
  auto second_descriptor = manifest.module("index_second_fts");
  ASSERT_TRUE(first_descriptor.has_value());
  ASSERT_TRUE(second_descriptor.has_value());
  auto first_path = first_descriptor->get_path("sqlite_fts_file");
  auto second_path = second_descriptor->get_path("sqlite_fts_file");
  ASSERT_TRUE(first_path.has_value());
  ASSERT_TRUE(second_path.has_value());
  EXPECT_NE(*first_path, *second_path);
  EXPECT_TRUE(std::filesystem::is_regular_file(*first_path));
  EXPECT_TRUE(std::filesystem::is_regular_file(*second_path));
}

}  // namespace
}  // namespace neug::sqlite_fts_ext
