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

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "neug/utils/mem_utils.h"

#include <arrow/array/array_base.h>
#include <arrow/compute/api_scalar.h>
#include <arrow/csv/reader.h>
#include <arrow/dataset/api.h>
#include <arrow/dataset/dataset.h>
#include <arrow/dataset/discovery.h>
#include <arrow/dataset/file_csv.h>
#include <arrow/dataset/scanner.h>
#include <arrow/filesystem/api.h>
#include <arrow/filesystem/filesystem.h>
#include <arrow/filesystem/localfs.h>
#include <arrow/filesystem/type_fwd.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <arrow/type.h>

#include "neug/compiler/common/assert.h"
#include "neug/execution/common/columns/arrow_context_column.h"
#include "neug/execution/common/context.h"
#include "neug/storages/loader/loader_utils.h"
#include "neug/utils/exception/exception.h"
#include "neug/utils/reader/options.h"

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

  // Check if streaming read mode is requested via environment variable
  const char* read_mode = getenv("NEUG_READ_MODE");
  if (read_mode && std::string(read_mode) == "streaming") {
    streaming_read(ctx);
    return;
  }

  auto scanner = createScanner(fileSystem);
  NEUG_ASSERT(scanner != nullptr);

  // Choose read mode: batch_read streams data, full_read loads entire dataset
  const auto& fileSchema = sharedState->schema.file;
  ReadOptions options;
  if (options.batch_read.get(fileSchema.options)) {
    batch_read(scanner, ctx);
  } else {
    full_read(scanner, ctx);
  }
}

std::shared_ptr<arrow::dataset::Scanner> ArrowReader::createScanner(
    std::shared_ptr<arrow::fs::FileSystem> fs) {
  neug::MemTracer mem_tracer("createScanner");

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
  mem_tracer.checkpoint("after_optionsBuilder_build");

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
  mem_tracer.checkpoint("after_project_and_filter");

  auto factory = datasetBuilder->buildFactory(sharedState, fs, fileFormat);
  mem_tracer.checkpoint("after_buildFactory");

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
  mem_tracer.checkpoint("after_dataset_Finish");

  // // Minimize readahead to reduce Arrow holding batch references in buffer
  // scan_opts->batch_readahead = 1;
  // scan_opts->fragment_readahead = 1;
  LOG(INFO) << "[DIAG] ScanOptions: batch_size=" << scan_opts->batch_size
            << " batch_readahead=" << scan_opts->batch_readahead
            << " fragment_readahead=" << scan_opts->fragment_readahead;
  arrow::dataset::ScannerBuilder scanner_builder(dataset, scan_opts);
  mem_tracer.checkpoint("after_ScannerBuilder_construct");

  auto scanner_result = scanner_builder.Finish();
  if (!scanner_result.ok()) {
    LOG(ERROR) << "Failed to create scanner: "
               << scanner_result.status().message();
    THROW_IO_EXCEPTION("Failed to create scanner: " +
                       scanner_result.status().message());
  }
  mem_tracer.checkpoint("after_ScannerBuilder_Finish");

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

  auto table_result = scanner->ToTable();
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

  output.clear();
  for (int i = 0; i < num_cols; ++i) {
    auto chunk_arrays = table->column(i)->chunks();
    execution::ArrowArrayContextColumnBuilder builder;
    for (const auto& array : chunk_arrays) {
      builder.push_back(array);
    }
    output.set(i, builder.finish());
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
  neug::MemTracer mem_tracer("batch_read");

  int64_t row_num = 0;
  // Experiment: test different CountRows strategies
  const char* diag_mode = getenv("NEUG_COUNT_MODE");
  if (diag_mode && std::string(diag_mode) == "skip") {
    LOG(INFO) << "[DIAG] Skipping CountRows";
    mem_tracer.checkpoint("skipped_CountRows");
  } else if (diag_mode && std::string(diag_mode) == "skip_sleep") {
    LOG(INFO) << "[DIAG] Skipping CountRows, sleeping 2s after ToRecordBatchReader";
    mem_tracer.checkpoint("skipped_CountRows");
  } else if (diag_mode && std::string(diag_mode) == "estimate") {
    // Estimate row count from file size (avg ~40 bytes per row for this schema)
    const auto& fileSchema = sharedState->schema.file;
    if (!fileSchema.paths.empty()) {
      auto fs_result = fileSystem->GetFileInfo(fileSchema.paths[0]);
      if (fs_result.ok()) {
        int64_t file_size = fs_result->size();
        row_num = file_size / 40;  // rough estimate
        LOG(INFO) << "[DIAG] Estimated row_num=" << row_num
                  << " from file_size=" << file_size;
      }
    }
    mem_tracer.checkpoint("after_estimate");
  } else {
    // Default: CountRows on same scanner
    auto row_num_result = scanner->CountRows();
    if (!row_num_result.ok()) {
      LOG(WARNING) << "Failed to count rows via scanner: "
                   << row_num_result.status().message();
      THROW_IO_EXCEPTION("Failed to count rows via scanner: " +
                         row_num_result.status().message());
    } else {
      VLOG(10) << "Row count from scanner: " << row_num_result.ValueOrDie();
      row_num = row_num_result.ValueOrDie();
    }
    mem_tracer.checkpoint("after_CountRows");
  }

  auto batch_reader_result = scanner->ToRecordBatchReader();
  if (!batch_reader_result.ok()) {
    LOG(ERROR) << "Failed to create RecordBatchReader from scanner: "
               << batch_reader_result.status().message();
    THROW_IO_EXCEPTION("Failed to create RecordBatchReader from scanner: " +
                       batch_reader_result.status().message());
  }
  auto batch_reader = batch_reader_result.ValueOrDie();
  mem_tracer.checkpoint("after_ToRecordBatchReader");

  // Diagnostic: sleep to let async scanner finish reading
  if (diag_mode && std::string(diag_mode) == "skip_sleep") {
    LOG(INFO) << "[DIAG] Sleeping 2s to let async scanner finish...";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    mem_tracer.checkpoint("after_sleep");
  }

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

namespace {
class StreamingReaderSupplier : public IRecordBatchSupplier {
 public:
  StreamingReaderSupplier(std::shared_ptr<arrow::csv::StreamingReader> reader,
                          int64_t row_num)
      : reader_(std::move(reader)), row_num_(row_num) {}

  std::shared_ptr<arrow::RecordBatch> GetNextBatch() override {
    auto result = reader_->Next();
    if (!result.ok() || *result == nullptr) {
      return nullptr;
    }
    return *result;
  }

  int64_t RowNum() const override { return row_num_; }

 private:
  std::shared_ptr<arrow::csv::StreamingReader> reader_;
  int64_t row_num_;
};
}  // namespace

void ArrowReader::streaming_read(execution::Context& output) {
  neug::MemTracer mem_tracer("streaming_read");

  if (!sharedState) {
    THROW_INVALID_ARGUMENT_EXCEPTION("SharedState is null");
  }
  if (!optionsBuilder) {
    THROW_INVALID_ARGUMENT_EXCEPTION("Options builder is null");
  }

  const auto& fileSchema = sharedState->schema.file;
  const std::vector<std::string>& file_paths = fileSchema.paths;
  if (file_paths.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION("No file paths provided");
  }

  // Estimate row count from file size
  int64_t row_num = 0;
  auto fs_result = fileSystem->GetFileInfo(file_paths[0]);
  if (fs_result.ok()) {
    int64_t file_size = fs_result->size();
    row_num = file_size / 40;  // rough estimate
    LOG(INFO) << "[DIAG:streaming_read] Estimated row_num=" << row_num
              << " from file_size=" << file_size;
  }
  mem_tracer.checkpoint("after_estimate");

  // Build fragment options to get read/parse/convert options
  auto arrowOptions = optionsBuilder->build();
  auto fragmentOpts = arrowOptions.scanOptions->fragment_scan_options;
  auto csvFragmentOpts =
      std::dynamic_pointer_cast<arrow::dataset::CsvFragmentScanOptions>(
          fragmentOpts);
  if (!csvFragmentOpts) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Failed to get CsvFragmentScanOptions for streaming read");
  }
  mem_tracer.checkpoint("after_build_options");

  auto read_options = csvFragmentOpts->read_options;
  auto parse_options = csvFragmentOpts->parse_options;
  auto convert_options = csvFragmentOpts->convert_options;

  // Open the file directly
  auto file_result = arrow::io::ReadableFile::Open(file_paths[0]);
  if (!file_result.ok()) {
    THROW_IO_EXCEPTION("Failed to open file: " +
                       file_result.status().message());
  }
  auto input_file = file_result.ValueOrDie();
  mem_tracer.checkpoint("after_open_file");

  // Create StreamingReader
  auto reader_result = arrow::csv::StreamingReader::Make(
      arrow::io::default_io_context(), input_file, read_options, parse_options,
      convert_options);
  if (!reader_result.ok()) {
    THROW_IO_EXCEPTION("Failed to create CSV StreamingReader: " +
                       reader_result.status().message());
  }
  auto streaming_reader = reader_result.ValueOrDie();
  mem_tracer.checkpoint("after_create_StreamingReader");

  LOG(INFO) << "[DIAG:streaming_read] StreamingReader created, block_size="
            << read_options.block_size;

  auto batch_supplier =
      std::make_shared<StreamingReaderSupplier>(streaming_reader, row_num);

  int num_cols = sharedState->columnNum();
  output.clear();
  for (int i = 0; i < num_cols; ++i) {
    execution::ArrowStreamContextColumnBuilder builder({batch_supplier});
    output.set(i, builder.finish());
  }
  mem_tracer.checkpoint("after_build_output");
}

arrow::Result<std::shared_ptr<arrow::Schema>> ArrowReader::inferSchema() {
  neug::MemTracer mem_tracer("inferSchema");

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
  mem_tracer.checkpoint("after_build_options");

  auto* pool = arrow::default_memory_pool();
  int64_t pool_before = pool->bytes_allocated();

  auto factory =
      datasetBuilder->buildFactory(sharedState, fileSystem, fileFormat);
  mem_tracer.checkpoint("after_buildFactory");

  // Infer schema using Inspect()
  auto result = factory->Inspect();
  int64_t pool_after = pool->bytes_allocated();
  mem_tracer.checkpoint("after_Inspect");
  LOG(INFO) << "[DIAG:inferSchema] pool_delta="
            << ((pool_after - pool_before) / 1048576.0) << " MB";
  return result;
}

}  // namespace reader
}  // namespace neug
