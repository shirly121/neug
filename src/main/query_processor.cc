/** Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "neug/main/query_processor.h"

#include <chrono>
#include <fstream>
#include <sstream>

#include "neug/execution/common/context.h"
#include "neug/execution/common/operators/retrieve/sink.h"
#include "neug/execution/execute/plan_parser.h"
#include "neug/main/neug_db.h"
#include "neug/storages/graph/property_graph.h"
#include "neug/utils/pb_utils.h"

namespace {

// Read current process RSS (Resident Set Size) in MB from /proc/self/status
double get_rss_mb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.compare(0, 6, "VmRSS:") == 0) {
      long kb = 0;
      std::sscanf(line.c_str(), "VmRSS: %ld kB", &kb);
      return kb / 1024.0;
    }
  }
  return -1.0;
}

}  // namespace

namespace neug {

result<std::pair<AccessMode, std::shared_ptr<execution::CacheValue>>>
QueryProcessor::check_and_retrieve_pipeline(const std::string& query_string,
                                            const std::string& user_access_mode,
                                            int32_t num_threads) {
  if (num_threads == 0) {
    num_threads = max_num_threads_;
  }
  if (num_threads > max_num_threads_) {
    num_threads = max_num_threads_;
  }
  if (num_threads < 1) {
    RETURN_ERROR(neug::Status(neug::StatusCode::ERR_INVALID_ARGUMENT,
                              "Number of threads must be greater than 0"));
  }

  auto access_mode = user_access_mode.empty()
                         ? planner_->analyzeMode(query_string)
                         : ParseAccessMode(user_access_mode);
  auto compile_start = std::chrono::high_resolution_clock::now();
  GS_AUTO(cache_value, global_query_cache_->Get(g_.schema(), query_string));
  auto compile_elapsed =
      std::chrono::duration<double>(std::chrono::high_resolution_clock::now() -
                                    compile_start)
          .count();
  LOG(INFO) << "Compile elapsed: " << compile_elapsed << " s";
  assert(cache_value);
  const auto& flags = cache_value->flags;
  if (is_read_only_) {
    if (flags.insert() || flags.update() || flags.schema() || flags.batch() ||
        flags.create_temp_table() || flags.checkpoint() ||
        flags.procedure_call()) {
      RETURN_ERROR(
          neug::Status(neug::StatusCode::ERR_INVALID_ARGUMENT,
                       "Write queries are not supported in read-only mode"));
    }
  }
  return std::make_pair(access_mode, cache_value);
}

result<QueryResult> QueryProcessor::execute(
    const std::string& query_string, const std::string& user_access_mode,
    const execution::ParamsMap& parameters, int32_t num_threads) {
  GS_AUTO(
      access_mode_pipeline,
      check_and_retrieve_pipeline(query_string, user_access_mode, num_threads));
  if (need_exclusive_lock(access_mode_pipeline.first)) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return execute_internal(query_string, access_mode_pipeline.second,
                            access_mode_pipeline.first, parameters,
                            num_threads);
  } else {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return execute_internal(query_string, access_mode_pipeline.second,
                            access_mode_pipeline.first, parameters,
                            num_threads);
  }
}

result<QueryResult> QueryProcessor::execute(const std::string& query_string,
                                            const std::string& user_access_mode,
                                            const rapidjson::Value& parameters,
                                            int32_t num_threads) {
  GS_AUTO(
      access_mode_pipeline,
      check_and_retrieve_pipeline(query_string, user_access_mode, num_threads));
  const auto& param_types = access_mode_pipeline.second->params_type;
  execution::ParamsMap params_map;
  if (parameters.IsObject()) {
    for (const auto& member : parameters.GetObject()) {
      std::string key = member.name.GetString();
      auto iter = param_types.find(key);
      if (iter == param_types.end()) {
        RETURN_ERROR(neug::Status(neug::StatusCode::ERR_INVALID_ARGUMENT,
                                  "Unexpected parameter: " + key));
      }
      params_map.emplace(
          key, execution::Value::FromJson(member.value, iter->second));
    }
  }
  if (need_exclusive_lock(access_mode_pipeline.first)) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return execute_internal(query_string, access_mode_pipeline.second,
                            access_mode_pipeline.first, params_map,
                            num_threads);
  } else {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return execute_internal(query_string, access_mode_pipeline.second,
                            access_mode_pipeline.first, params_map,
                            num_threads);
  }
}

// The concurrency control is done outside this function.
result<QueryResult> QueryProcessor::execute_internal(
    const std::string& query_string,
    std::shared_ptr<execution::CacheValue> cache_value, AccessMode access_mode,
    const execution::ParamsMap& parameters, int32_t num_threads) {
  StorageAPUpdateInterface graph(g_, 0, allocator_);
  auto timer_ptr = std::make_unique<execution::OprTimer>();
  double mem0 = get_rss_mb();
  auto t0 = std::chrono::high_resolution_clock::now();
  auto ctx_res = cache_value->pipeline.Execute(graph, execution::Context(),
                                               parameters, timer_ptr.get());
  auto t1 = std::chrono::high_resolution_clock::now();
  double mem1 = get_rss_mb();
  if (!ctx_res) {
    LOG(ERROR) << "Error in executing query: " << query_string
               << ", error code: " << ctx_res.error().error_code()
               << ", message: " << ctx_res.error().error_message();
    RETURN_ERROR(ctx_res.error());
  }

  google::protobuf::Arena arena;
  neug::QueryResponse* response =
      google::protobuf::Arena::CreateMessage<neug::QueryResponse>(&arena);
  neug::execution::Sink::sink_results(ctx_res.value(), graph, response);
  auto t2 = std::chrono::high_resolution_clock::now();
  double mem2 = get_rss_mb();
  response->mutable_schema()->CopyFrom(cache_value->result_schema);
  QueryResult ret = QueryResult::From(response->SerializeAsString());
  auto t3 = std::chrono::high_resolution_clock::now();
  double mem3 = get_rss_mb();
  const auto& responsePB = ret.response();
  // SpaceUsedLong: protobuf's approximate recursive heap for this message tree
  // (owned buffers, submessages, repeated fields). Not RSS; not wire size.
  const size_t heap_bytes = responsePB.SpaceUsedLong();
  const size_t wire_bytes = static_cast<size_t>(responsePB.ByteSizeLong());
  LOG(INFO) << "QueryResponse heap (SpaceUsedLong): " << heap_bytes
            << " bytes (" << (heap_bytes / 1048576.0)
            << " MB); wire (ByteSizeLong): " << wire_bytes << " bytes ("
            << (wire_bytes / 1048576.0) << " MB)";

  update_compiler_meta_if_needed(cache_value->flags, access_mode);
  auto t4 = std::chrono::high_resolution_clock::now();

  {
    std::ostringstream oss;
    timer_ptr->output("  ", oss);
    auto sec = [](auto a, auto b) {
      return std::chrono::duration<double>(b - a).count();
    };
    LOG(INFO) << "Query: " << query_string;
    LOG(INFO) << "  pipeline.Execute : " << sec(t0, t1) << " s";
    LOG(INFO) << "  sink_results     : " << sec(t1, t2) << " s";
    LOG(INFO) << "  serialize        : " << sec(t2, t3) << " s";
    LOG(INFO) << "  update_meta      : " << sec(t3, t4) << " s";
    LOG(INFO) << "  total            : " << sec(t0, t4) << " s";
    LOG(INFO) << "Memory (RSS):";
    LOG(INFO) << "  before execute   : " << mem0 << " MB";
    LOG(INFO) << "  after execute    : " << mem1 << " MB (+" << mem1 - mem0
              << ")";
    LOG(INFO) << "  after sink       : " << mem2 << " MB (+" << mem2 - mem1
              << ")";
    LOG(INFO) << "  after serialize  : " << mem3 << " MB (+" << mem3 - mem2
              << ")";
    LOG(INFO) << "Operator timing:\n" << oss.str();
  }

  return ret;
}

bool QueryProcessor::need_exclusive_lock(AccessMode access_mode) {
  if (access_mode == AccessMode::kRead) {
    return false;
  }
  return true;  // For Insert and Update operations
}

void QueryProcessor::update_compiler_meta_if_needed(
    const physical::ExecutionFlag& flags, AccessMode mode) {
  YAML::Node schema_yaml;
  std::string statistics_json;
  bool need_update = false;
  if (flags.schema() || flags.create_temp_table() ||
      mode == AccessMode::kSchema) {
    schema_yaml = g_.schema().to_yaml().value();
    need_update = true;
  }
  if (flags.batch() || flags.insert() || flags.update()) {
    statistics_json = g_.get_statistics_json();
    need_update = true;
  }
  if (need_update) {
    global_query_cache_->clear(schema_yaml, statistics_json);
  }
}

}  // namespace neug
