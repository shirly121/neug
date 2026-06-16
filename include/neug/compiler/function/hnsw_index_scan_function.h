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

#pragma once

#include <string>
#include <vector>

#include "neug/compiler/function/neug_call_function.h"
#include "neug/storages/index/i_index.h"

namespace neug {
namespace function {

struct HNSWIndexScanFuncInput : public CallFuncInputBase {
  std::string index_name;
  label_t label_id = 0;
  std::vector<float> target_vec;
  int topK = 0;
  MetricType metric_type = MetricType::L2;
};

struct HNSWIndexScanFunction {
  static constexpr const char* name = "HNSW_INDEX_SCAN";

  static function_set getFunctionSet();
};

}  // namespace function
}  // namespace neug
