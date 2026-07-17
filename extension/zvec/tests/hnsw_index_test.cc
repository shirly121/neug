#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "hnsw_index.h"
#include "neug/main/connection.h"
#include "neug/main/neug_db.h"
#include "neug/storages/checkpoint.h"
#include "neug/storages/checkpoint_manifest.h"
#include "neug/storages/module/module_broker.h"
#include "neug/storages/module/module_factory.h"
#include "neug/utils/property/array_column.h"
#include "neug/utils/property/vec_column.h"
#include "vector_distance_function.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace neug::zvec_ext {
namespace {

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
  const auto extension_path =
      std::filesystem::path("extension/zvec/libzvec.neug_extension");
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

neug::Value MakeFloatArray(std::initializer_list<float> values) {
  std::vector<neug::Value> children;
  children.reserve(values.size());
  for (auto value : values) {
    children.push_back(neug::Value::FLOAT(value));
  }
  return neug::Value::ARRAY(
      neug::DataType::Array(neug::DataType::FLOAT, values.size()),
      std::move(children));
}

TEST(VectorDistanceFunctionTest, L2Distance) {
  auto lhs = MakeFloatArray({1.0f, 2.0f, 3.0f});
  auto rhs = MakeFloatArray({1.0f, 4.0f, 5.0f});
  auto result = neug::zvec_ext::VectorDistanceL2Function::Exec({lhs, rhs});
  EXPECT_DOUBLE_EQ(result.GetValue<double>(), std::sqrt(8.0));
}

TEST(VectorDistanceFunctionTest, CosineDistance) {
  auto lhs = MakeFloatArray({1.0f, 0.0f});
  auto rhs = MakeFloatArray({0.0f, 2.0f});
  auto result = neug::zvec_ext::VectorDistanceCosineFunction::Exec({lhs, rhs});
  EXPECT_DOUBLE_EQ(result.GetValue<double>(), 1.0);
}

TEST(VectorDistanceFunctionTest, InnerProduct) {
  auto lhs = MakeFloatArray({1.0f, 2.0f, 3.0f});
  auto rhs = MakeFloatArray({2.0f, 3.0f, 4.0f});
  auto result = neug::zvec_ext::VectorDistanceIPFunction::Exec({lhs, rhs});
  EXPECT_DOUBLE_EQ(result.GetValue<double>(), 20.0);
}

TEST(VectorDistanceFunctionTest, CypherPropertyAndArrayLiteral) {
  auto build_root = FindBuildRoot();
  ASSERT_FALSE(build_root.empty());
  setenv("NEUG_EXTENSION_HOME_PYENV", build_root.c_str(), 1);

  auto database_path =
      std::filesystem::temp_directory_path() / "neug_zvec_distance_function";
  std::filesystem::remove_all(database_path);

  neug::NeugDB database;
  ASSERT_TRUE(database.Open(database_path));
  auto connection = database.Connect();
  ASSERT_NE(connection, nullptr);

  auto load = connection->Query("LOAD zvec;");
  ASSERT_TRUE(load.has_value()) << load.error().ToString();
  auto schema = connection->Query(
      "CREATE NODE TABLE Item(id INT64 PRIMARY KEY, "
      "name STRING, vec FLOAT[3]);");
  ASSERT_TRUE(schema.has_value()) << schema.error().ToString();
  auto insert = connection->Query(
      "CREATE (:Item {id: 1, name: 'alice', "
      "vec: [1.0, 2.0, 3.0]});");
  ASSERT_TRUE(insert.has_value()) << insert.error().ToString();

  auto result = connection->Query(
      "MATCH (n:Item) RETURN vector_distance_l2(n.vec, [1.0, 4.0, "
      "5.0]), vector_distance_cosine(n.vec, [1.0, 2.0, 3.0]), "
      "vector_distance_ip(n.vec, [2.0, 3.0, 4.0]);");
  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  ASSERT_EQ(result->response().arrays_size(), 3);
  EXPECT_DOUBLE_EQ(result->response().arrays(0).double_array().values(0),
                   std::sqrt(8.0));
  EXPECT_NEAR(result->response().arrays(1).double_array().values(0), 0.0,
              1e-12);
  EXPECT_DOUBLE_EQ(result->response().arrays(2).double_array().values(0), 20.0);

  auto insert_more = connection->Query(
      "CREATE (:Item {id: 2, name: 'marko', vec: [2.0, 2.0, 2.0]}), "
      "(:Item {id: 3, name: 'carol', vec: [3.0, 3.0, 3.0]}), "
      "(:Item {id: 4, name: 'dave', vec: [4.0, 4.0, 4.0]});");
  ASSERT_TRUE(insert_more.has_value()) << insert_more.error().ToString();
  auto create_index = connection->Query(
      "CREATE INDEX item_vec_hnsw ON Item USING HNSW (vec) "
      "WITH (metric = 'l2', m = 16, ef_construction = 100);");
  ASSERT_TRUE(create_index.has_value()) << create_index.error().ToString();

  auto index_scan = connection->Query(
      "MATCH (n:Item) "
      "RETURN n.id, vector_distance_l2(n.vec, [2.1, 2.1, 2.1]) AS score "
      "ORDER BY score ASC LIMIT 2;");
  ASSERT_TRUE(index_scan.has_value()) << index_scan.error().ToString();
  ASSERT_EQ(index_scan->length(), 2);
  ASSERT_EQ(index_scan->response().arrays_size(), 2);
  ASSERT_EQ(index_scan->response().arrays(1).double_array().values_size(), 2);
  EXPECT_EQ(index_scan->response().arrays(0).int64_array().values(0), 2);
  EXPECT_EQ(index_scan->response().arrays(0).int64_array().values(1), 1);
  EXPECT_NEAR(index_scan->response().arrays(1).double_array().values(0),
              std::sqrt(0.03), 1e-6);
  EXPECT_NEAR(index_scan->response().arrays(1).double_array().values(1),
              std::sqrt(2.03), 1e-6);

  auto filtered_index_scan = connection->Query(
      "MATCH (n:Item) WHERE n.name <> 'marko' "
      "RETURN n.id, vector_distance_l2(n.vec, [2.1, 2.1, 2.1]) AS score "
      "ORDER BY score ASC LIMIT 2;");
  ASSERT_TRUE(filtered_index_scan.has_value())
      << filtered_index_scan.error().ToString();
  ASSERT_EQ(filtered_index_scan->length(), 2);
  EXPECT_EQ(filtered_index_scan->response().arrays(0).int64_array().values(0),
            1);
  EXPECT_EQ(filtered_index_scan->response().arrays(0).int64_array().values(1),
            3);
  EXPECT_NEAR(
      filtered_index_scan->response().arrays(1).double_array().values(0),
      std::sqrt(2.03), 1e-6);
  EXPECT_NEAR(
      filtered_index_scan->response().arrays(1).double_array().values(1),
      std::sqrt(2.43), 1e-6);

  auto empty_filter = connection->Query(
      "MATCH (n:Item) WHERE n.name = 'nobody' "
      "RETURN n.id, vector_distance_l2(n.vec, [2.1, 2.1, 2.1]) AS score "
      "ORDER BY score ASC LIMIT 2;");
  ASSERT_TRUE(empty_filter.has_value()) << empty_filter.error().ToString();
  EXPECT_EQ(empty_filter->length(), 0);

  auto ip_schema = connection->Query(
      "CREATE NODE TABLE IPItem(id INT64 PRIMARY KEY, vec FLOAT[3]);");
  ASSERT_TRUE(ip_schema.has_value()) << ip_schema.error().ToString();
  auto ip_insert = connection->Query(
      "CREATE (:IPItem {id: 1, vec: [1.0, 0.0, 0.0]}), "
      "(:IPItem {id: 2, vec: [1.0, 2.0, 0.0]}), "
      "(:IPItem {id: 3, vec: [-1.0, -1.0, 0.0]}), "
      "(:IPItem {id: 4, vec: [2.0, 3.0, 0.0]});");
  ASSERT_TRUE(ip_insert.has_value()) << ip_insert.error().ToString();
  auto create_ip_index = connection->Query(
      "CREATE INDEX ip_item_vec_hnsw ON IPItem USING HNSW (vec) "
      "WITH (metric = 'ip', m = 16, ef_construction = 100);");
  ASSERT_TRUE(create_ip_index.has_value())
      << create_ip_index.error().ToString();

  auto ip_index_scan = connection->Query(
      "MATCH (n:IPItem) "
      "RETURN n.id, vector_distance_ip(n.vec, [1.0, 1.0, 0.0]) AS score "
      "ORDER BY score DESC LIMIT 3;");
  ASSERT_TRUE(ip_index_scan.has_value()) << ip_index_scan.error().ToString();
  ASSERT_EQ(ip_index_scan->length(), 3);
  ASSERT_EQ(ip_index_scan->response().arrays_size(), 2);
  EXPECT_EQ(ip_index_scan->response().arrays(0).int64_array().values(0), 4);
  EXPECT_EQ(ip_index_scan->response().arrays(0).int64_array().values(1), 2);
  EXPECT_EQ(ip_index_scan->response().arrays(0).int64_array().values(2), 1);
  EXPECT_DOUBLE_EQ(ip_index_scan->response().arrays(1).double_array().values(0),
                   5.0);
  EXPECT_DOUBLE_EQ(ip_index_scan->response().arrays(1).double_array().values(1),
                   3.0);
  EXPECT_DOUBLE_EQ(ip_index_scan->response().arrays(1).double_array().values(2),
                   1.0);

  connection.reset();
  database.Close();
  std::filesystem::remove_all(database_path);
}

constexpr const char* kIndexKey = "index";
constexpr const char* kColumnKey = "column";

std::unique_ptr<IndexMeta> MakeHNSWMeta(const std::string& name, int dimension,
                                        const std::string& metric = "l2",
                                        int m = 16, int ef_construction = 200) {
  auto meta = std::make_unique<IndexMeta>();
  meta->name = name;
  meta->type = "HNSW";
  meta->schema.label_id = 0;
  meta->schema.property_name = "embedding";
  meta->schema.property_type = DataType::Array(DataType::FLOAT, dimension);
  meta->options["dimension"] = std::to_string(dimension);
  meta->options["metric"] = metric;
  meta->options["m"] = std::to_string(m);
  meta->options["ef_construction"] = std::to_string(ef_construction);
  return meta;
}

Value MakeVectorValue(int dimension, float value) {
  auto type = DataType::Array(DataType::FLOAT, dimension);
  std::vector<Value> elements;
  elements.reserve(dimension);
  for (int i = 0; i < dimension; ++i) {
    elements.emplace_back(Value::FLOAT(value));
  }
  return Value::ARRAY(type, std::move(elements));
}

std::unique_ptr<VecColumn> MakeVecColumn(Checkpoint& checkpoint, int dimension,
                                         size_t size) {
  auto type = DataType::Array(DataType::FLOAT, dimension);
  auto buffer = std::make_shared<ArrayColumn>(type);
  buffer->Open(checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory);
  auto accessor = std::make_unique<DefaultIndexIDAccessor>();
  accessor->Open(checkpoint, ModuleDescriptor{}, MemoryLevel::kInMemory);
  auto column =
      std::make_unique<VecColumn>(std::move(buffer), std::move(accessor));
  column->resize(size);
  return column;
}

std::unique_ptr<HNSWIndex> MakeIndex(Checkpoint& checkpoint,
                                     std::unique_ptr<IndexMeta> meta,
                                     const ModuleDescriptor& descriptor,
                                     const VecColumn& column) {
  auto index = std::make_unique<HNSWIndex>();
  auto status =
      index->Init(std::move(meta), std::make_unique<DefaultIndexIDAccessor>());
  EXPECT_TRUE(status.ok()) << status.error_message();
  index->Open(checkpoint, descriptor, MemoryLevel::kInMemory);
  status = index->Rebind(IndexBindContext{&column});
  EXPECT_TRUE(status.ok()) << status.error_message();
  return index;
}

std::unique_ptr<HNSWIndex> ReopenIndex(Checkpoint& checkpoint,
                                       const ModuleDescriptor& descriptor,
                                       const VecColumn& column) {
  auto index = std::make_unique<HNSWIndex>();
  index->Open(checkpoint, descriptor, MemoryLevel::kInMemory);
  auto status = index->Rebind(IndexBindContext{&column});
  EXPECT_TRUE(status.ok()) << status.error_message();
  return index;
}

CheckpointManifest DumpState(Checkpoint& checkpoint, HNSWIndex& index,
                             VecColumn& column) {
  CheckpointManifest manifest;
  column.Dump(checkpoint, manifest, kColumnKey);
  index.Dump(checkpoint, manifest, kIndexKey);
  return manifest;
}

std::unique_ptr<VecColumn> ReopenColumn(Checkpoint& checkpoint,
                                        const CheckpointManifest& manifest) {
  auto descriptor = manifest.module(kColumnKey);
  EXPECT_TRUE(descriptor.has_value());
  auto column = std::make_unique<VecColumn>();
  column->Open(checkpoint, manifest, *descriptor, MemoryLevel::kInMemory);
  return column;
}

std::vector<vid_t> Search(HNSWIndex& index, int dimension, float query_value,
                          uint32_t topk) {
  HNSWIndexQueryParams params;
  params.target_value = MakeVectorValue(dimension, query_value);
  params.topk = topk;
  params.ef_search = std::max<uint32_t>(100, topk);
  auto results = index.Search(params);
  EXPECT_TRUE(results.has_value()) << results.error().error_message();
  return results ? std::move(results.value()) : std::vector<vid_t>{};
}

void Append(HNSWIndex& index, VecColumn& column, vid_t vid, int dimension,
            float value) {
  auto vector = MakeVectorValue(dimension, value);
  column.set_any(vid, vector, true);
  auto status = index.Upsert(vid, vector);
  ASSERT_TRUE(status.ok()) << status.error_message();
}

void ExpectVector(const VecColumn& column, vid_t vid, int dimension,
                  float expected) {
  auto offset = column.get_offset(vid);
  ASSERT_NE(offset, INVALID_INDEX_ID);
  const auto* data = static_cast<const float*>(
      column.get_buffer()->get_row_ptr<float>(offset));
  ASSERT_NE(data, nullptr);
  for (int i = 0; i < dimension; ++i) {
    EXPECT_FLOAT_EQ(data[i], expected)
        << "vid=" << vid << ", dimension=" << i << ", offset=" << offset;
  }
}

void ExpectArray(const ArrayColumn& column, size_t row, int dimension,
                 float expected) {
  const auto* data = static_cast<const float*>(column.get_row_ptr<float>(row));
  ASSERT_NE(data, nullptr);
  for (int i = 0; i < dimension; ++i) {
    EXPECT_FLOAT_EQ(data[i], expected) << "row=" << row << ", dimension=" << i;
  }
}

class HNSWIndexLifecycleTest : public ::testing::Test {
 protected:
  static constexpr int kDimension = 16;

  void SetUp() override {
    tmp_dir_ = std::filesystem::temp_directory_path() / "neug_zvec_lifecycle";
    std::filesystem::remove_all(tmp_dir_);
    checkpoint_ = OpenCheckpoint(0);
  }

  void TearDown() override {
    checkpoint_.reset();
    std::filesystem::remove_all(tmp_dir_);
  }

  std::filesystem::path tmp_dir_;
  std::shared_ptr<Checkpoint> checkpoint_;

  std::shared_ptr<Checkpoint> OpenCheckpoint(int id) {
    return Checkpoint::Open(
        (tmp_dir_ / ("checkpoint-" + std::to_string(id))).string(), id);
  }
};

TEST_F(HNSWIndexLifecycleTest, ArrayColumnEmptyDumpReopenAppendDumpReopen) {
  constexpr size_t kRowCount = 8;
  constexpr const char* kArrayColumnKey = "array_column";
  CheckpointManifest manifest;
  auto checkpoint_1 = OpenCheckpoint(1);
  auto checkpoint_2 = OpenCheckpoint(2);

  {
    ArrayColumn column(DataType::Array(DataType::FLOAT, kDimension));
    column.Open(*checkpoint_, ModuleDescriptor{}, MemoryLevel::kInMemory);
    column.resize(kRowCount);
    column.Dump(*checkpoint_1, manifest, kArrayColumnKey);
  }

  {
    auto descriptor = manifest.module(kArrayColumnKey);
    ASSERT_TRUE(descriptor.has_value());
    ArrayColumn column;
    column.Open(*checkpoint_1, manifest, *descriptor, MemoryLevel::kInMemory);

    for (size_t row = 0; row < kRowCount; ++row) {
      column.set_any(row, MakeVectorValue(kDimension, static_cast<float>(row)),
                     true);
      SCOPED_TRACE("after append");
      ExpectArray(column, row, kDimension, static_cast<float>(row));
    }

    CheckpointManifest updated_manifest;
    column.Dump(*checkpoint_2, updated_manifest, kArrayColumnKey);
    manifest = std::move(updated_manifest);
  }

  {
    auto descriptor = manifest.module(kArrayColumnKey);
    ASSERT_TRUE(descriptor.has_value());
    ArrayColumn column;
    column.Open(*checkpoint_2, manifest, *descriptor, MemoryLevel::kInMemory);

    for (size_t row = 0; row < kRowCount; ++row) {
      SCOPED_TRACE("after second reopen");
      ExpectArray(column, row, kDimension, static_cast<float>(row));
    }
  }
}

TEST_F(HNSWIndexLifecycleTest, OpenDumpReopenAppendDumpSearch) {
  CheckpointManifest manifest;
  auto checkpoint_1 = OpenCheckpoint(1);
  auto checkpoint_2 = OpenCheckpoint(2);

  {
    auto column = MakeVecColumn(*checkpoint_, kDimension, 8);
    auto index =
        MakeIndex(*checkpoint_, MakeHNSWMeta("lifecycle_test", kDimension),
                  ModuleDescriptor{}, *column);

    const auto& meta = index->GetMeta();
    EXPECT_EQ(meta.name, "lifecycle_test");
    EXPECT_EQ(meta.type, "HNSW");
    EXPECT_EQ(meta.options.at("dimension"), std::to_string(kDimension));

    manifest = DumpState(*checkpoint_1, *index, *column);
    ASSERT_TRUE(manifest.module(kIndexKey)->has("index_meta"));
  }

  {
    auto column = ReopenColumn(*checkpoint_1, manifest);
    auto index =
        ReopenIndex(*checkpoint_1, *manifest.module(kIndexKey), *column);
    for (vid_t vid = 0; vid < 8; ++vid) {
      Append(*index, *column, vid, kDimension, static_cast<float>(vid));
      SCOPED_TRACE("after append");
      ExpectVector(*column, vid, kDimension, static_cast<float>(vid));
    }
    manifest = DumpState(*checkpoint_2, *index, *column);
  }

  {
    auto column = ReopenColumn(*checkpoint_2, manifest);
    auto index =
        ReopenIndex(*checkpoint_2, *manifest.module(kIndexKey), *column);
    for (vid_t vid = 0; vid < 8; ++vid) {
      SCOPED_TRACE("after second reopen");
      ExpectVector(*column, vid, kDimension, static_cast<float>(vid));
    }
    auto results = Search(*index, kDimension, 2.1f, 8);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0], 2u);
  }
}

class HNSWIndexAdvancedTest : public ::testing::Test {
 protected:
  static constexpr int kDimension = 16;
  static constexpr int kNumVectors = 1000;

  static void SetUpTestSuite() {
    tmp_dir_ = std::filesystem::temp_directory_path() / "neug_zvec_advanced";
    std::filesystem::remove_all(tmp_dir_);
    auto source_checkpoint =
        Checkpoint::Open((tmp_dir_ / "checkpoint-0").string(), 0);
    checkpoint_ = Checkpoint::Open((tmp_dir_ / "checkpoint-1").string(), 1);

    auto column = MakeVecColumn(*source_checkpoint, kDimension, kNumVectors);
    auto index =
        MakeIndex(*source_checkpoint, MakeHNSWMeta("advanced_test", kDimension),
                  ModuleDescriptor{}, *column);
    for (vid_t vid = 0; vid < kNumVectors; ++vid) {
      Append(*index, *column, vid, kDimension, static_cast<float>(vid));
    }
    manifest_ = DumpState(*checkpoint_, *index, *column);
  }

  static void TearDownTestSuite() {
    checkpoint_.reset();
    std::filesystem::remove_all(tmp_dir_);
  }

  void SetUp() override {
    column_ = ReopenColumn(*checkpoint_, manifest_);
    index_ = ReopenIndex(*checkpoint_, *manifest_.module(kIndexKey), *column_);
  }

  std::vector<vid_t> DoSearch(float query_value, uint32_t topk = 10) {
    return Search(*index_, kDimension, query_value, topk);
  }

  static std::filesystem::path tmp_dir_;
  static std::shared_ptr<Checkpoint> checkpoint_;
  static CheckpointManifest manifest_;
  std::unique_ptr<VecColumn> column_;
  std::unique_ptr<HNSWIndex> index_;
};

std::filesystem::path HNSWIndexAdvancedTest::tmp_dir_;
std::shared_ptr<Checkpoint> HNSWIndexAdvancedTest::checkpoint_;
CheckpointManifest HNSWIndexAdvancedTest::manifest_;

TEST_F(HNSWIndexAdvancedTest, SearchOrdering) {
  auto results = DoSearch(3.1f);
  ASSERT_GE(results.size(), 3u);
  EXPECT_EQ(results[0], 3u);
  EXPECT_EQ(results[1], 4u);
  EXPECT_EQ(results[2], 2u);
}

TEST_F(HNSWIndexAdvancedTest, SearchExactMatch) {
  auto results = DoSearch(0.0f);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0], 0u);
}

TEST_F(HNSWIndexAdvancedTest, SearchMiddle) {
  auto results = DoSearch(500.0f);
  ASSERT_GE(results.size(), 3u);
  EXPECT_EQ(results[0], 500u);
  EXPECT_EQ(results[1], 501u);
  EXPECT_EQ(results[2], 499u);
}

TEST_F(HNSWIndexAdvancedTest, SearchBoundary) {
  auto results = DoSearch(999.0f);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0], 999u);
}

TEST_F(HNSWIndexAdvancedTest, DeleteAndSearch) {
  auto status = index_->Delete(3);
  ASSERT_TRUE(status.ok()) << status.error_message();

  auto results = DoSearch(3.1f);
  ASSERT_FALSE(results.empty());
  for (auto vid : results) {
    EXPECT_NE(vid, 3u);
  }
  EXPECT_TRUE(results[0] == 4u || results[0] == 2u);
}

TEST_F(HNSWIndexAdvancedTest, MetadataRoundtrip) {
  const auto& meta = index_->GetMeta();
  EXPECT_EQ(meta.name, "advanced_test");
  EXPECT_EQ(meta.type, "HNSW");
  EXPECT_EQ(meta.options.at("dimension"), std::to_string(kDimension));
  EXPECT_EQ(meta.options.at("metric"), "l2");
}

TEST_F(HNSWIndexAdvancedTest, ModuleBrokerRestore) {
  CheckpointManifest index_manifest;
  index_manifest.set_module(kIndexKey, *manifest_.module(kIndexKey));

  ModuleBroker broker;
  broker.Open(*checkpoint_, index_manifest, MemoryLevel::kInMemory);
  auto index = broker.TakeModule<HNSWIndex>(kIndexKey);
  ASSERT_NE(index, nullptr);

  const auto& meta = index->GetMeta();
  EXPECT_EQ(meta.name, "advanced_test");
  EXPECT_EQ(meta.type, "HNSW");
  EXPECT_EQ(meta.options.at("dimension"), std::to_string(kDimension));
  EXPECT_EQ(meta.options.at("metric"), "l2");
}

}  // namespace
}  // namespace neug::zvec_ext
