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

#include "neug/compiler/function/hnsw_index_scan_function.h"

#include <rapidjson/document.h>
#include <glog/logging.h>
#include <limits>
#include <string>

#include "neug/execution/common/columns/vertex_columns.h"
#include "neug/execution/common/context.h"
#include "neug/generated/proto/plan/physical.pb.h"
#include "neug/storages/graph/graph_interface.h"
#include "neug/storages/index/index_manager.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace function {

namespace {

std::string getStringMember(const rapidjson::Value& obj, const char* name,
                            const std::string& fallback = "") {
  if (obj.HasMember(name) && obj[name].IsString()) {
    return obj[name].GetString();
  }
  return fallback;
}

int getIntMember(const rapidjson::Value& obj, const char* name, int fallback) {
  if (obj.HasMember(name) && obj[name].IsInt()) {
    return obj[name].GetInt();
  }
  return fallback;
}

MetricType parseMetricType(const std::string& metric) {
  if (metric == "cosine" || metric == "COSINE") {
    return MetricType::COSINE;
  }
  if (metric == "ip" || metric == "IP" || metric == "inner_product" ||
      metric == "INNER_PRODUCT") {
    return MetricType::INNER_PRODUCT;
  }
  return MetricType::L2;
}

std::vector<float> parseTargetVector(const rapidjson::Value& obj) {
  std::vector<float> result;
  if (!obj.HasMember("target_vec") || !obj["target_vec"].IsArray()) {
    return result;
  }
  for (const auto& item : obj["target_vec"].GetArray()) {
    if (!item.IsNumber()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "HNSW_INDEX_SCAN target_vec must contain only numbers");
    }
    result.push_back(static_cast<float>(item.GetDouble()));
  }
  return result;
}

std::string firstJsonArgument(const ::physical::PhysicalPlan& plan,
                              int op_idx) {
  const auto& args =
      plan.plan(op_idx).opr().procedure_call().query().arguments();
  if (args.empty() || !args.Get(0).has_const_() ||
      args.Get(0).const_().item_case() != ::common::Value::kStr) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "HNSW_INDEX_SCAN expects one JSON string argument");
  }
  return args.Get(0).const_().str();
}

}  // namespace

function_set HNSWIndexScanFunction::getFunctionSet() {
  auto function = std::make_unique<NeugCallFunction>(
      HNSWIndexScanFunction::name,
      std::vector<neug::common::LogicalTypeID>{neug::common::LogicalTypeID::STRING},
      std::vector<std::pair<std::string, neug::common::LogicalTypeID>>{
          {"n", neug::common::LogicalTypeID::NODE}});

  function->bindFunc = [](const neug::Schema& schema,
                          const neug::execution::ContextMeta& ctx_meta,
                          const ::physical::PhysicalPlan& plan,
                          int op_idx) -> std::unique_ptr<CallFuncInputBase> {
    auto input = std::make_unique<HNSWIndexScanFuncInput>();
    auto json = firstJsonArgument(plan, op_idx);

    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "HNSW_INDEX_SCAN argument must be a JSON object");
    }

    input->index_name = getStringMember(doc, "index_name");
    if (input->index_name.empty()) {
      input->index_name = getStringMember(doc, "unique_name");
    }
    if (input->index_name.empty()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "HNSW_INDEX_SCAN argument requires index_name");
    }

    auto labelName = getStringMember(doc, "label");
    if (!labelName.empty()) {
      input->label_id = schema.get_vertex_label_id(labelName);
    } else if (doc.HasMember("label_id") && doc["label_id"].IsUint()) {
      input->label_id = static_cast<label_t>(doc["label_id"].GetUint());
    }

    input->target_vec = parseTargetVector(doc);
    if (input->target_vec.empty()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "HNSW_INDEX_SCAN argument requires non-empty target_vec");
    }
    input->topK = getIntMember(doc, "topK", getIntMember(doc, "top_k", 0));
    if (input->topK <= 0) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "HNSW_INDEX_SCAN argument requires positive topK");
    }
    input->metric_type =
        parseMetricType(getStringMember(doc, "metric_type", "l2"));
    return input;
  };

  function->execFunc =
      [](const CallFuncInputBase& inputBase, neug::IStorageInterface& graph) {
        const auto& input =
            common::neug_dynamic_cast<const HNSWIndexScanFuncInput&>(
                inputBase);
        auto* index = graph.index_manager().GetIndexByName(input.index_name);
        if (index == nullptr) {
          THROW_INVALID_ARGUMENT_EXCEPTION("Index not found: " +
                                           input.index_name);
        }

        HNSWIndexQueryParams queryParams;
        queryParams.target_vec = input.target_vec;
        queryParams.topK = input.topK;
        queryParams.metric_type = input.metric_type;

        IndexFilterParams filterParams;
        std::vector<vid_t> vids;
        auto status = index->Search(queryParams, filterParams, vids);
        if (!status.ok()) {
          THROW_RUNTIME_ERROR("HNSW_INDEX_SCAN search failed: " +
                              status.error_message());
        }

        neug::execution::MSVertexColumnBuilder builder(input.label_id);
        builder.reserve(vids.size());
        for (auto vid : vids) {
          if (vid == std::numeric_limits<vid_t>::max()) {
            continue;
          }
          builder.push_back_opt(vid);
        }

        neug::execution::Context ctx;
        ctx.set(0, builder.finish());
        ctx.tag_ids = {0};
        return ctx;
      };

  function_set functionSet;
  functionSet.push_back(std::move(function));
  return functionSet;
}

}  // namespace function
}  // namespace neug
