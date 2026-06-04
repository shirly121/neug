/** Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "hnsw_index.h"
#include "neug/storages/graph/graph_interface.h"
#include "neug/storages/index/i_index.h"
#include "neug/storages/index/index_factory.h"
#include "neug/utils/property/property.h"

namespace neug::extension::zvec_ext {
namespace {

/// Helper: pack raw float array into a Property as string_view.
static neug::Property MakeVectorProperty(const float* data, int dim) {
  neug::Property p;
  p.set_string_view(std::string_view(reinterpret_cast<const char*>(data),
                                     dim * sizeof(float)));
  return p;
}

/// Build an IndexMeta with HNSW options from individual params.
static neug::IndexMeta MakeHNSWMeta(const std::string& name, int dimension,
                                    const std::string& metric = "l2",
                                    int m = 16, int ef = 200) {
  neug::IndexMeta meta;
  meta.name = name;
  meta.type = "HNSW";
  meta.options["dimension"] = std::to_string(dimension);
  meta.options["metric"] = metric;
  meta.options["m"] = std::to_string(m);
  meta.options["ef_construction"] = std::to_string(ef);
  return meta;
}

/// Minimal IStorageInterface stub for tests (no actual graph).
class StubStorageInterface : public neug::IStorageInterface {
 public:
  bool readable() const override { return false; }
  bool writable() const override { return false; }
  const neug::Schema& schema() const override {
    static neug::Schema s;
    return s;
  }
  bool GetVertexIndex(neug::label_t, const neug::Property&,
                      neug::vid_t&) const override {
    return false;
  }
};

class HNSWIndexTest : public ::testing::Test {
 protected:
  static constexpr int kDimension = 128;
  static constexpr int kNumVectors = 100;
  static constexpr int kTopK = 10;

  void SetUp() override {
    tmp_dir_ = std::filesystem::temp_directory_path() / "neug_zvec_test";
    std::filesystem::create_directories(tmp_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

  static std::vector<float> RandomVector(int dim) {
    std::vector<float> v(dim);
    for (int i = 0; i < dim; ++i) {
      v[i] = static_cast<float>(rand()) / RAND_MAX;
    }
    return v;
  }

  StubStorageInterface stub_storage_;
  std::filesystem::path tmp_dir_;
};

TEST_F(HNSWIndexTest, CreateAndAppend) {
  auto meta = MakeHNSWMeta("test", kDimension);
  HNSWIndex index("test", meta, stub_storage_);

  // Append vectors
  for (int i = 0; i < kNumVectors; ++i) {
    auto vec = RandomVector(kDimension);
    auto status = index.Append(static_cast<vid_t>(i),
                               {MakeVectorProperty(vec.data(), kDimension)});
    ASSERT_TRUE(status.ok()) << status.error_message();
  }
}

TEST_F(HNSWIndexTest, SearchReturnsResults) {
  auto meta = MakeHNSWMeta("test", kDimension);
  HNSWIndex index("test", meta, stub_storage_);

  std::vector<std::vector<float>> vectors;
  for (int i = 0; i < kNumVectors; ++i) {
    vectors.push_back(RandomVector(kDimension));
    auto status =
        index.Append(static_cast<vid_t>(i),
                     {MakeVectorProperty(vectors.back().data(), kDimension)});
    ASSERT_TRUE(status.ok());
  }

  // Search with the first vector as query
  HNSWIndexQueryParams params;
  params.query_vector = vectors[0].data();
  params.dimension = kDimension;
  params.topk = kTopK;

  neug::IndexFilterParams filter_params;
  std::vector<vid_t> results;

  auto status = index.Search(params, filter_params, results);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_GT(results.size(), 0u);
  ASSERT_LE(results.size(), static_cast<size_t>(kTopK));

  // The nearest neighbor of vectors[0] should be itself (vid=0)
  EXPECT_EQ(results[0], 0u);
}

TEST_F(HNSWIndexTest, DeleteExcludesFromSearch) {
  auto meta = MakeHNSWMeta("test", kDimension);
  HNSWIndex index("test", meta, stub_storage_);

  std::vector<std::vector<float>> vectors;
  for (int i = 0; i < kNumVectors; ++i) {
    vectors.push_back(RandomVector(kDimension));
    index.Append(static_cast<vid_t>(i),
                 {MakeVectorProperty(vectors.back().data(), kDimension)});
  }

  // Delete vid 0
  index.Delete(static_cast<vid_t>(0));

  // Search for vectors[0] -- vid 0 should not appear
  HNSWIndexQueryParams params;
  params.query_vector = vectors[0].data();
  params.dimension = kDimension;
  params.topk = kTopK;

  neug::IndexFilterParams filter_params;
  std::vector<vid_t> results;
  auto status = index.Search(params, filter_params, results);
  ASSERT_TRUE(status.ok());

  for (auto vid : results) {
    EXPECT_NE(vid, 0u);
  }
}

TEST_F(HNSWIndexTest, MetadataIsCorrect) {
  auto meta = MakeHNSWMeta("test_meta", kDimension);
  HNSWIndex index("test_meta", meta, stub_storage_);

  const auto& returned_meta = index.GetMeta();
  EXPECT_EQ(returned_meta.type, "HNSW");
  EXPECT_EQ(returned_meta.name, "test_meta");
}

TEST_F(HNSWIndexTest, ForkSharesState) {
  auto meta = MakeHNSWMeta("fork_test", kDimension);
  HNSWIndex index("fork_test", meta, stub_storage_);

  // Insert some vectors
  for (int i = 0; i < 100; ++i) {
    auto vec = RandomVector(kDimension);
    index.Append(static_cast<vid_t>(i),
                 {MakeVectorProperty(vec.data(), kDimension)});
  }

  // Fork
  auto forked = index.Fork();
  ASSERT_NE(forked, nullptr);
  EXPECT_EQ(forked->GetMeta().name, "fork_test");
}

TEST_F(HNSWIndexTest, IndexFactoryRegistration) {
  neug::IndexFactory::Instance().RegisterCreator(
      "HNSW",
      [](const neug::ModuleDescriptor &
         /*desc*/) -> std::unique_ptr<neug::Index> {
        return std::make_unique<HNSWIndex>();
      });

  EXPECT_TRUE(neug::IndexFactory::Instance().HasType("HNSW"));

  neug::ModuleDescriptor desc;
  desc.set("dimension", "64");
  desc.set("metric", "l2");
  auto index = neug::IndexFactory::Instance().Create("HNSW", desc);
  ASSERT_NE(index, nullptr);
}

}  // namespace
}  // namespace neug::extension::zvec_ext
