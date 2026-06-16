/**
 * Copyright 2020 Alibaba Group Holding Limited.
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

#include "gopt_test.h"
#include "neug/compiler/catalog/catalog_entry/catalog_entry_type.h"
#include "neug/compiler/function/table/table_function.h"
#include "neug/compiler/transaction/transaction.h"

namespace neug {
namespace gopt {

class CreateIndexTest : public GOptTest {
 public:
  std::string getDDLResource(std::string resource) {
    return getGOptResource("ddl_test/" + resource);
  };

  void SetUp() override {
    GOptTest::SetUp();

    // Load schema (includes VectorNode with ARRAY<FLOAT,128>) and stats
    std::string schemaData =
        getGOptResource("schema/create_follows_schema.yaml");
    std::string statsData =
        getGOptResource("stats/create_follows_stats.json");
    database->updateSchema(schemaData);
    database->updateStats(statsData);

    // Register CREATE_HNSW_INDEX function in catalog
    auto* catalog = getCatalog();
    function::function_set funcSet;
    auto func = std::make_unique<function::TableFunction>(
        "CREATE_HNSW_INDEX", std::vector<common::LogicalTypeID>{});
    func->computeSignature();
    funcSet.push_back(std::move(func));
    catalog->addFunction(&transaction::DUMMY_TRANSACTION,
                         catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY,
                         "CREATE_HNSW_INDEX", std::move(funcSet));
  }
};

TEST_F(CreateIndexTest, CREATE_INDEX_BASIC) {
  std::string query =
      "CREATE INDEX vec_hnsw_index ON VectorNode USING HNSW (embedding);";
  auto logical = planLogical(query);
  auto physical = planPhysical(*logical);
  VerifyFactory::verifyPhysicalByJson(
      *physical, getDDLResource("CREATE_INDEX_BASIC_physical"));
}

TEST_F(CreateIndexTest, CREATE_INDEX_WITH_OPTIONS) {
  std::string query =
      "CREATE INDEX vec_hnsw_index ON VectorNode USING HNSW (embedding) WITH "
      "(metric = 'cosine');";
  auto logical = planLogical(query);
  auto physical = planPhysical(*logical);
  VerifyFactory::verifyPhysicalByJson(
      *physical, getDDLResource("CREATE_INDEX_WITH_OPTIONS_physical"));
}

TEST_F(CreateIndexTest, CREATE_INDEX_IF_NOT_EXISTS) {
  std::string query =
      "CREATE INDEX IF NOT EXISTS vec_hnsw_index ON VectorNode USING HNSW "
      "(embedding);";
  auto logical = planLogical(query);
  auto physical = planPhysical(*logical);
  VerifyFactory::verifyPhysicalByJson(
      *physical, getDDLResource("CREATE_INDEX_IF_NOT_EXISTS_physical"));
}

TEST_F(CreateIndexTest, CREATE_INDEX_MULTI_COLS) {
  std::string query =
      "CREATE INDEX vec_hnsw_index ON VectorNode USING HNSW (embedding, "
      "label);";
  auto logical = planLogical(query);
  auto physical = planPhysical(*logical);
  VerifyFactory::verifyPhysicalByJson(
      *physical, getDDLResource("CREATE_INDEX_MULTI_COLS_physical"));
}

}  // namespace gopt
}  // namespace neug
