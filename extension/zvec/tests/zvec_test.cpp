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

#include <filesystem>
#include <vector>

#include "hnsw_index.h"
#include "neug/storages/checkpoint.h"
#include "neug/storages/checkpoint_manifest.h"
#include "neug/storages/index/i_index.h"
#include "neug/storages/module/module_broker.h"
#include "neug/storages/module/module_factory.h"
#include "neug/utils/property/property.h"

namespace neug::extension::zvec_ext {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static neug::Property MakeVectorProperty(const float* data, int dim) {
  neug::Property p;
  p.set_string_view(std::string_view(reinterpret_cast<const char*>(data),
                                     dim * sizeof(float)));
  return p;
}

static std::unique_ptr<neug::IndexMeta> MakeHNSWMeta(
    const std::string& name, int dimension, const std::string& metric = "l2",
    int m = 16, int ef = 200) {
  auto meta = std::make_unique<neug::IndexMeta>();
  meta->name = name;
  meta->type = "HNSW";
  meta->options["dimension"] = std::to_string(dimension);
  meta->options["metric"] = metric;
  meta->options["m"] = std::to_string(m);
  meta->options["ef_construction"] = std::to_string(ef);
  return meta;
}

static std::vector<float> MakeVector(int dim, float value) {
  return std::vector<float>(dim, value);
}

// ---------------------------------------------------------------------------
// Suite 1: Lifecycle — Open → Dump → reOpen → Append → Dump → Search
//
// A single test that verifies the full sequential chain, so it can be
// run independently without depending on other tests.
// ---------------------------------------------------------------------------

class HNSWIndexLifecycleTest : public ::testing::Test {
 protected:
  static constexpr int kDimension = 16;

  std::filesystem::path tmp_dir_;
  std::shared_ptr<neug::Checkpoint> ckp_;

  void SetUp() override {
    tmp_dir_ = std::filesystem::temp_directory_path() / "neug_zvec_lifecycle";
    std::filesystem::remove_all(tmp_dir_);
    std::filesystem::create_directories(tmp_dir_);
    auto ckp_dir = tmp_dir_ / "checkpoint-0";
    ckp_ = neug::Checkpoint::Open(ckp_dir.string(), 0);
  }

  void TearDown() override {
    ckp_.reset();
    std::filesystem::remove_all(tmp_dir_);
  }
};

TEST_F(HNSWIndexLifecycleTest, OpenDumpAppendSearch) {
  // -- Step 1: Create fresh index, verify meta, dump -------------------------
  neug::ModuleDescriptor desc;
  {
    auto meta = MakeHNSWMeta("lifecycle_test", kDimension);
    HNSWIndex index("lifecycle_test", std::move(meta));

    neug::ModuleDescriptor empty_desc;
    ASSERT_NO_THROW(
        index.Open(*ckp_, empty_desc, neug::MemoryLevel::kInMemory));

    const auto& m = index.GetMeta();
    EXPECT_EQ(m.name, "lifecycle_test");
    EXPECT_EQ(m.type, "HNSW");
    EXPECT_EQ(m.options.at("dimension"), std::to_string(kDimension));

    desc = index.Dump(*ckp_);
    ASSERT_TRUE(desc.has("index_meta"));
  }

  // -- Step 2: Reopen from dump, append vids [0,1,2,3], dump -----------------
  {
    HNSWIndex index;
    index.Open(*ckp_, desc, neug::MemoryLevel::kInMemory);

    for (int i = 0; i < 4; ++i) {
      auto vec = MakeVector(kDimension, static_cast<float>(i));
      auto status = index.Append(static_cast<vid_t>(i),
                                 {MakeVectorProperty(vec.data(), kDimension)});
      ASSERT_TRUE(status.ok()) << status.error_message();
    }

    desc = index.Dump(*ckp_);

    // Verify data can be reopened
    HNSWIndex verify_index;
    ASSERT_NO_THROW(
        verify_index.Open(*ckp_, desc, neug::MemoryLevel::kInMemory));
  }

  // -- Step 3: Reopen from dump, search [2.1,...], verify nearest=vid 2 ------
  {
    HNSWIndex index;
    index.Open(*ckp_, desc, neug::MemoryLevel::kInMemory);

    auto query = MakeVector(kDimension, 2.1f);
    HNSWIndexQueryParams params;
    params.query_vector = query.data();
    params.dimension = kDimension;
    params.topk = 4;

    neug::IndexFilterParams filter_params;
    std::vector<vid_t> results;
    auto status = index.Search(params, filter_params, results);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_GE(results.size(), 1u);
    EXPECT_EQ(results[0], 2u)
        << "nearest to [2.1,...] should be vid=2 [2.0,...]";
  }
}

// ---------------------------------------------------------------------------
// Suite 2: Advanced — larger dataset with multiple search patterns
//
// SetUpTestSuite inserts 1000 vectors (vid i -> [i, i, ...]) and dumps.
// Each test opens independently from the same dumped descriptor.
// ---------------------------------------------------------------------------

class HNSWIndexAdvancedTest : public ::testing::Test {
 protected:
  static constexpr int kDimension = 16;
  static constexpr int kNumVectors = 1000;

  static std::filesystem::path tmp_dir_;
  static std::shared_ptr<neug::Checkpoint> ckp_;
  static neug::ModuleDescriptor last_desc_;

  static void SetUpTestSuite() {
    tmp_dir_ = std::filesystem::temp_directory_path() / "neug_zvec_advanced";
    std::filesystem::remove_all(tmp_dir_);
    std::filesystem::create_directories(tmp_dir_);
    auto ckp_dir = tmp_dir_ / "checkpoint-0";
    ckp_ = neug::Checkpoint::Open(ckp_dir.string(), 0);

    // Build index with 1000 deterministic vectors
    auto meta = MakeHNSWMeta("advanced_test", kDimension);
    HNSWIndex index("advanced_test", std::move(meta));

    neug::ModuleDescriptor empty_desc;
    index.Open(*ckp_, empty_desc, neug::MemoryLevel::kInMemory);

    for (int i = 0; i < kNumVectors; ++i) {
      auto vec = MakeVector(kDimension, static_cast<float>(i));
      auto status = index.Append(static_cast<vid_t>(i),
                                 {MakeVectorProperty(vec.data(), kDimension)});
      ASSERT_TRUE(status.ok()) << status.error_message();
    }

    last_desc_ = index.Dump(*ckp_);
  }

  static void TearDownTestSuite() {
    ckp_.reset();
    std::filesystem::remove_all(tmp_dir_);
  }

  std::unique_ptr<HNSWIndex> OpenIndex() {
    auto index = std::make_unique<HNSWIndex>();
    index->Open(*ckp_, last_desc_, neug::MemoryLevel::kInMemory);
    return index;
  }

  std::vector<vid_t> DoSearch(HNSWIndex& index, float query_value, int topk) {
    auto query = MakeVector(kDimension, query_value);
    HNSWIndexQueryParams params;
    params.query_vector = query.data();
    params.dimension = kDimension;
    params.topk = topk;

    neug::IndexFilterParams filter_params;
    std::vector<vid_t> results;
    auto status = index.Search(params, filter_params, results);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return results;
  }
};

std::filesystem::path HNSWIndexAdvancedTest::tmp_dir_;
std::shared_ptr<neug::Checkpoint> HNSWIndexAdvancedTest::ckp_;
neug::ModuleDescriptor HNSWIndexAdvancedTest::last_desc_;

// Search [3.1, ...]: nearest vid=3, then vid=4, then vid=2
// L2sq distances: d(3.1,3)=16*0.01=0.16, d(3.1,4)=16*0.81=12.96,
//                 d(3.1,2)=16*1.21=19.36
TEST_F(HNSWIndexAdvancedTest, SearchOrdering) {
  auto index = OpenIndex();
  auto results = DoSearch(*index, 3.1f, 10);

  ASSERT_GE(results.size(), 3u);
  EXPECT_EQ(results[0], 3u);
  EXPECT_EQ(results[1], 4u);
  EXPECT_EQ(results[2], 2u);
}

// Search [0.0, ...]: nearest vid=0
TEST_F(HNSWIndexAdvancedTest, SearchExactMatch) {
  auto index = OpenIndex();
  auto results = DoSearch(*index, 0.0f, 10);

  ASSERT_GE(results.size(), 1u);
  EXPECT_EQ(results[0], 0u);
}

// Search [500.0, ...]: nearest vid=500, then 501, then 499
TEST_F(HNSWIndexAdvancedTest, SearchMiddle) {
  auto index = OpenIndex();
  auto results = DoSearch(*index, 500.0f, 10);

  ASSERT_GE(results.size(), 3u);
  EXPECT_EQ(results[0], 500u);
  EXPECT_EQ(results[1], 501u);
  EXPECT_EQ(results[2], 499u);
}

// Search [999.0, ...]: nearest vid=999 (boundary)
TEST_F(HNSWIndexAdvancedTest, SearchBoundary) {
  auto index = OpenIndex();
  auto results = DoSearch(*index, 999.0f, 10);

  ASSERT_GE(results.size(), 1u);
  EXPECT_EQ(results[0], 999u);
}

// Delete vid 3, search [3.1, ...]: vid 3 must not appear in results
TEST_F(HNSWIndexAdvancedTest, DeleteAndSearch) {
  auto index = OpenIndex();

  auto status = index->Delete(static_cast<vid_t>(3));
  ASSERT_TRUE(status.ok()) << status.error_message();

  auto results = DoSearch(*index, 3.1f, 10);
  ASSERT_GE(results.size(), 1u);

  for (auto vid : results) {
    EXPECT_NE(vid, 3u) << "deleted vid should not appear in results";
  }
  // After deleting 3, nearest to 3.1 should be 4 (d=12.96) or 2 (d=19.36)
  EXPECT_TRUE(results[0] == 4u || results[0] == 2u);
}

// Metadata roundtrip: constructor sets meta, Open preserves it
TEST_F(HNSWIndexAdvancedTest, MetadataRoundtrip) {
  auto index = OpenIndex();

  const auto& meta = index->GetMeta();
  EXPECT_EQ(meta.name, "advanced_test");
  EXPECT_EQ(meta.type, "HNSW");
  EXPECT_EQ(meta.options.at("dimension"), std::to_string(kDimension));
  EXPECT_EQ(meta.options.at("metric"), "l2");
}

// Restore index via ModuleBroker from checkpoint descriptor, verify meta
TEST_F(HNSWIndexAdvancedTest, ModuleBrokerRestore) {
  neug::ModuleFactory::instance().Register<HNSWIndex>();

  neug::CheckpointManifest manifest;
  manifest.set_module("index_advanced_test", last_desc_);

  neug::ModuleBroker broker;
  broker.Open(*ckp_, manifest, neug::MemoryLevel::kInMemory);

  auto index = broker.TakeModule<neug::Index>("index_advanced_test");
  ASSERT_NE(index, nullptr);

  const auto& meta = index->GetMeta();
  EXPECT_EQ(meta.name, "advanced_test");
  EXPECT_EQ(meta.type, "HNSW");
  EXPECT_EQ(meta.options.at("dimension"), std::to_string(kDimension));
  EXPECT_EQ(meta.options.at("metric"), "l2");
}

}  // namespace
}  // namespace neug::extension::zvec_ext
