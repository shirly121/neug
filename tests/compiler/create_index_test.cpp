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

namespace neug {
namespace gopt {

class CreateIndexTest : public GOptTest {
 public:
  std::string schemaData = getGOptResource("schema/create_follows_schema.yaml");
  std::string statsData = getGOptResource("stats/create_follows_stats.json");
  std::string getDDLResource(std::string resource) {
    return getGOptResource("ddl_test/" + resource);
  };

  std::vector<std::string> rules = {"FilterPushDown", "ExpandGetVFusion"};
};

TEST_F(CreateIndexTest, CREATE_INDEX_BASIC) {
  std::string query = "CREATE INDEX vec_hnsw_index ON User USING HNSW (age);";
  auto logical = planLogical(query, schemaData, statsData, rules);
  auto physical = planPhysical(*logical);
  VerifyFactory::verifyPhysicalByJson(
      *physical, getDDLResource("CREATE_INDEX_BASIC_physical"));
}

TEST_F(CreateIndexTest, CREATE_INDEX_WITH_OPTIONS) {
  std::string query =
      "CREATE INDEX vec_hnsw_index ON User USING HNSW (age) WITH (metric = "
      "'cosine');";
  auto logical = planLogical(query, schemaData, statsData, rules);
  auto physical = planPhysical(*logical);
  VerifyFactory::verifyPhysicalByJson(
      *physical, getDDLResource("CREATE_INDEX_WITH_OPTIONS_physical"));
}

TEST_F(CreateIndexTest, CREATE_INDEX_IF_NOT_EXISTS) {
  std::string query =
      "CREATE INDEX IF NOT EXISTS vec_hnsw_index ON User USING HNSW (age);";
  auto logical = planLogical(query, schemaData, statsData, rules);
  auto physical = planPhysical(*logical);
  VerifyFactory::verifyPhysicalByJson(
      *physical, getDDLResource("CREATE_INDEX_IF_NOT_EXISTS_physical"));
}

TEST_F(CreateIndexTest, CREATE_INDEX_MULTI_COLS) {
  std::string query =
      "CREATE INDEX vec_hnsw_index ON User USING HNSW (name, age);";
  auto logical = planLogical(query, schemaData, statsData, rules);
  auto physical = planPhysical(*logical);
  VerifyFactory::verifyPhysicalByJson(
      *physical, getDDLResource("CREATE_INDEX_MULTI_COLS_physical"));
}

}  // namespace gopt
}  // namespace neug
