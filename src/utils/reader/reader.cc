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

#include <arrow/result.h>
#include <arrow/status.h>
#include <glog/logging.h>

#include "neug/utils/reader/reader.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <arrow/array/array_base.h>
#include <arrow/compute/api_scalar.h>
#include <arrow/dataset/api.h>
#include <arrow/dataset/dataset.h>
#include <arrow/dataset/discovery.h>
#include <arrow/dataset/file_csv.h>
#include <arrow/dataset/scanner.h>
#include <arrow/filesystem/api.h>
#include <arrow/filesystem/filesystem.h>
#include <arrow/filesystem/localfs.h>
#include <arrow/filesystem/type_fwd.h>
#include <arrow/table.h>
#include <arrow/type.h>

#include "neug/compiler/common/assert.h"
#include "neug/execution/common/columns/arrow_context_column.h"
#include "neug/execution/common/context.h"
#include "neug/storages/loader/loader_utils.h"
#include "neug/utils/exception/exception.h"
#include "neug/utils/reader/options.h"

namespace {

// Process RSS (VmRSS) in MB; -1 if unavailable (non-Linux / no /proc).
double GetRssMb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.compare(0, 6, "VmRSS:") == 0) {
      long kb = 0;
      if (std::sscanf(line.c_str(), "VmRSS: %ld kB", &kb) == 1) {
        return kb / 1024.0;
      }
    }
  }
  return -1.0;
}

// Sum Arrow buffer bytes for ArrayData (recursive for nested types).
int64_t ArrayDataBufferBytes(const arrow::ArrayData& data) {
  int64_t n = 0;
  for (const auto& buf : data.buffers) {
    if (buf) {
      n += static_cast<int64_t>(buf->size());
    }
  }
  for (const auto& child : data.child_data) {
    if (child) {
      n += ArrayDataBufferBytes(*child);
    }
  }
  return n;
}

int64_t ArrayBufferBytes(const arrow::Array& arr) {
  if (!arr.data()) {
    return 0;
  }
  return ArrayDataBufferBytes(*arr.data());
}

// Best-effort in-process buffer footprint of table columns (not allocator
// overhead, dict indices elsewhere, or pages not reflected in buffers).
int64_t EstimateTableBufferBytes(const arrow::Table& table) {
  int64_t total = 0;
  for (int c = 0; c < table.num_columns(); ++c) {
    const auto& col = table.column(c);
    for (const auto& chunk : col->chunks()) {
      total += ArrayBufferBytes(*chunk);
    }
  }
  return total;
}

}  // namespace

namespace neug {
namespace reader {

void ArrowReader::read(std::shared_ptr<ReadLocalState> localState,
                       execution::Context& ctx) {
  if (!sharedState) {
    THROW_INVALID_ARGUMENT_EXCEPTION("SharedState is null");
  }

  if (!fileSystem) {
    THROW_INVALID_ARGUMENT_EXCEPTION("FileSystem is null");
  }

  auto scanner = createScanner(fileSystem);
  NEUG_ASSERT(scanner != nullptr);

  // Choose read mode: batch_read streams data, full_read loads entire dataset
  const auto& fileSchema = sharedState->schema.file;
  ReadOptions options;
  bool read_flag = options.batch_read.get(fileSchema.options);
  LOG(INFO) << "ArrowReader::read read_flag: " << read_flag;
  if (read_flag) {
    batch_read(scanner, ctx);
  } else {
    full_read(scanner, ctx);
  }
}

std::shared_ptr<arrow::dataset::Scanner> ArrowReader::createScanner(
    std::shared_ptr<arrow::fs::FileSystem> fs) {
  if (!fs) {
    THROW_INVALID_ARGUMENT_EXCEPTION("FileSystem is null");
  }

  if (!sharedState) {
    THROW_INVALID_ARGUMENT_EXCEPTION("SharedState is null");
  }

  const auto& fileSchema = sharedState->schema.file;
  const std::vector<std::string>& file_paths = fileSchema.paths;

  if (file_paths.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION("No file paths provided");
  }

  if (!optionsBuilder) {
    THROW_INVALID_ARGUMENT_EXCEPTION("Options builder is null");
  }

  auto arrowOptions = optionsBuilder->build();
  if (!arrowOptions.scanOptions) {
    THROW_INVALID_ARGUMENT_EXCEPTION("Failed to build arrow options");
  }

  if (!optionsBuilder->projectColumns(arrowOptions)) {
    LOG(WARNING) << "Failed to set column projection, using all columns";
  }

  if (!optionsBuilder->skipRows(arrowOptions)) {
    LOG(WARNING) << "Failed to set row filter, using no filter";
  }

  auto scan_opts = arrowOptions.scanOptions;
  auto fileFormat = arrowOptions.fileFormat;
  if (!fileFormat) {
    LOG(ERROR) << "File format is null in arrow options";
    THROW_INVALID_ARGUMENT_EXCEPTION("File format is null in arrow options");
  }

  auto factory = datasetBuilder->buildFactory(sharedState, fs, fileFormat);

  arrow::Result<std::shared_ptr<arrow::dataset::Dataset>> dataset_result;
  if (scan_opts->dataset_schema) {
    dataset_result = factory->Finish(scan_opts->dataset_schema);
  } else {
    arrow::dataset::FinishOptions finish_options;
    finish_options.validate_fragments = false;
    dataset_result = factory->Finish(finish_options);
  }
  if (!dataset_result.ok()) {
    LOG(ERROR) << "Failed to create dataset from factory: "
               << dataset_result.status().message();
    THROW_IO_EXCEPTION("Failed to create dataset from factory: " +
                       dataset_result.status().message());
  }
  auto dataset = dataset_result.ValueOrDie();

  arrow::dataset::ScannerBuilder scanner_builder(dataset, scan_opts);
  auto scanner_result = scanner_builder.Finish();
  if (!scanner_result.ok()) {
    LOG(ERROR) << "Failed to create scanner: "
               << scanner_result.status().message();
    THROW_IO_EXCEPTION("Failed to create scanner: " +
                       scanner_result.status().message());
  }
  return scanner_result.ValueOrDie();
}

void ArrowReader::full_read(std::shared_ptr<arrow::dataset::Scanner> scanner,
                            execution::Context& output) {
  if (!sharedState) {
    THROW_INVALID_ARGUMENT_EXCEPTION("SharedState is null");
  }
  if (!scanner) {
    THROW_INVALID_ARGUMENT_EXCEPTION("Scanner is null");
  }

  const double rss_before_scan = GetRssMb();
  auto table_result = scanner->ToTable();
  const double rss_after_scan = GetRssMb();
  if (!table_result.ok()) {
    LOG(ERROR) << "Failed to read table via scanner: "
               << table_result.status().message();
    THROW_IO_EXCEPTION("Failed to read table via scanner: " +
                       table_result.status().message());
  }
  auto table = table_result.ValueOrDie();

  int num_cols = sharedState->columnNum();
  if (num_cols != table->num_columns()) {
    THROW_IO_EXCEPTION(
        "Column number mismatch between schema and table, schema: " +
        std::to_string(num_cols) +
        ", table: " + std::to_string(table->num_columns()));
  }

  const int64_t est_table_bytes = EstimateTableBufferBytes(*table);
  if (rss_before_scan >= 0 && rss_after_scan >= 0) {
    LOG(INFO) << "ArrowReader::full_read RSS: before ToTable="
              << rss_before_scan << " MB, after ToTable=" << rss_after_scan
              << " MB, delta=" << (rss_after_scan - rss_before_scan) << " MB";
  } else {
    LOG(INFO) << "ArrowReader::full_read RSS: unavailable (no /proc VmRSS)";
  }
  LOG(INFO) << "ArrowReader::full_read table footprint: rows="
            << table->num_rows() << " cols=" << table->num_columns()
            << ", estimated column buffers=" << (est_table_bytes / 1048576.0)
            << " MB (" << est_table_bytes << " bytes)";

  for (int i = 0; i < table->num_columns(); ++i) {
    int64_t col_bytes = 0;
    for (const auto& chunk : table->column(i)->chunks()) {
      col_bytes += ArrayBufferBytes(*chunk);
    }
    const auto& field = table->schema()->field(i);
    LOG(INFO) << "  column[" << i << "] " << field->name() << " "
              << field->type()->ToString() << " ~" << (col_bytes / 1048576.0)
              << " MB (" << col_bytes << " bytes)";
  }

  output.clear();
  for (int i = 0; i < num_cols; ++i) {
    auto chunk_arrays = table->column(i)->chunks();
    execution::ArrowArrayContextColumnBuilder builder;
    for (const auto& array : chunk_arrays) {
      builder.push_back(array);
    }
    output.set(i, builder.finish());
  }

  const double rss_after_ctx = GetRssMb();
  if (rss_after_scan >= 0 && rss_after_ctx >= 0) {
    LOG(INFO) << "ArrowReader::full_read RSS: after Context build="
              << rss_after_ctx
              << " MB, delta=" << (rss_after_ctx - rss_after_scan)
              << " MB (Context wraps same arrays; delta often ~0)";
  }
}

void ArrowReader::batch_read(std::shared_ptr<arrow::dataset::Scanner> scanner,
                             execution::Context& output) {
  if (!sharedState) {
    THROW_INVALID_ARGUMENT_EXCEPTION("SharedState is null");
  }
  if (!scanner) {
    THROW_INVALID_ARGUMENT_EXCEPTION("Scanner is null");
  }
  auto row_num_result = scanner->CountRows();
  int64_t row_num = 0;
  if (!row_num_result.ok()) {
    LOG(WARNING) << "Failed to count rows via scanner: "
                 << row_num_result.status().message();
    THROW_IO_EXCEPTION("Failed to count rows via scanner: " +
                       row_num_result.status().message());
  } else {
    VLOG(10) << "Row count from scanner: " << row_num_result.ValueOrDie();
    row_num = row_num_result.ValueOrDie();
  }

  auto batch_reader_result = scanner->ToRecordBatchReader();
  if (!batch_reader_result.ok()) {
    LOG(ERROR) << "Failed to create RecordBatchReader from scanner: "
               << batch_reader_result.status().message();
    THROW_IO_EXCEPTION("Failed to create RecordBatchReader from scanner: " +
                       batch_reader_result.status().message());
  }
  auto batch_reader = batch_reader_result.ValueOrDie();

  auto batch_supplier = std::make_shared<neug::ArrowRecordBatchStreamSupplier>(
      batch_reader, row_num);

  int num_cols = sharedState->columnNum();
  output.clear();
  for (int i = 0; i < num_cols; ++i) {
    // NOTE: Each column uses the same shared RecordBatch supplier, so columns
    // share access to entire batches rather than being split by column. This
    // design may need refactoring when storage no longer relies on Arrow.
    execution::ArrowStreamContextColumnBuilder builder({batch_supplier});
    output.set(i, builder.finish());
  }
}

arrow::Result<std::shared_ptr<arrow::Schema>> ArrowReader::inferSchema() {
  if (!sharedState) {
    return arrow::Status::Invalid(neug::StatusCode::ERR_INVALID_ARGUMENT,
                                  "SharedState is null");
  }

  if (!fileSystem) {
    return arrow::Status::Invalid(neug::StatusCode::ERR_INVALID_ARGUMENT,
                                  "FileSystem is null");
  }

  if (!optionsBuilder) {
    return arrow::Status::Invalid(neug::StatusCode::ERR_INVALID_ARGUMENT,
                                  "Options builder is null");
  }

  // Reuse optionsBuilder->build() to get fileFormat
  // For schema inference, we need fileFormat but don't need entry schema.
  // build() will create an empty dataset_schema if entry schema is empty,
  // but fileFormat will still be correctly built.
  auto arrowOptions = optionsBuilder->build();
  if (!arrowOptions.fileFormat) {
    return arrow::Status::IOError(
        "Failed to build file format from options builder");
  }
  auto fileFormat = arrowOptions.fileFormat;

  auto factory =
      datasetBuilder->buildFactory(sharedState, fileSystem, fileFormat);

  // Infer schema using Inspect()
  return factory->Inspect();
}

}  // namespace reader
}  // namespace neug
