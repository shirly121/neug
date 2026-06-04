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

#include <gtest/gtest.h>

#include <arrow/api.h>
#include <arrow/array/builder_nested.h>
#include <arrow/dataset/file_parquet.h>
#include <arrow/filesystem/localfs.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <parquet/arrow/writer.h>

#include <filesystem>
#include <memory>
#include <vector>

#include "../../extension/parquet/include/parquet_options.h"
#include "neug/execution/common/columns/arrow_context_column.h"
#include "neug/execution/common/context.h"
#include "neug/generated/proto/plan/basic_type.pb.h"
#include "neug/utils/reader/options.h"
#include "neug/utils/reader/reader.h"
#include "neug/utils/reader/schema.h"
#include "neug/utils/reader/type_converter.h"

namespace neug {
namespace test {

static constexpr const char* PARQUET_TEST_DIR = "/tmp/parquet_list_test";

class ParquetListTest : public ::testing::Test {
 public:
  void SetUp() override {
    if (std::filesystem::exists(PARQUET_TEST_DIR)) {
      std::filesystem::remove_all(PARQUET_TEST_DIR);
    }
    std::filesystem::create_directories(PARQUET_TEST_DIR);
  }

  void TearDown() override {
    if (std::filesystem::exists(PARQUET_TEST_DIR)) {
      std::filesystem::remove_all(PARQUET_TEST_DIR);
    }
  }

  // Write an Arrow table to a Parquet file
  void writeParquetFile(const std::string& filename,
                        const std::shared_ptr<arrow::Table>& table) {
    std::string path = std::string(PARQUET_TEST_DIR) + "/" + filename;
    auto outfile = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    ASSERT_OK(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(),
                                         outfile, /*chunk_size=*/1024));
  }

  // Create shared state for Parquet reading
  std::shared_ptr<reader::ReadSharedState> createParquetSharedState(
      const std::string& filename, const std::vector<std::string>& columnNames,
      const std::vector<std::shared_ptr<::common::DataType>>& columnTypes) {
    auto sharedState = std::make_shared<reader::ReadSharedState>();

    auto entrySchema = std::make_shared<reader::TableEntrySchema>();
    entrySchema->columnNames = columnNames;
    entrySchema->columnTypes = columnTypes;

    reader::FileSchema fileSchema;
    fileSchema.paths = {std::string(PARQUET_TEST_DIR) + "/" + filename};
    fileSchema.format = "parquet";
    fileSchema.options = {{"batch_read", "false"}};

    reader::ExternalSchema externalSchema;
    externalSchema.entry = entrySchema;
    externalSchema.file = fileSchema;

    sharedState->schema = std::move(externalSchema);
    return sharedState;
  }

  std::shared_ptr<reader::ArrowReader> createParquetReader(
      const std::shared_ptr<reader::ReadSharedState>& sharedState) {
    auto fileSystem = std::make_shared<arrow::fs::LocalFileSystem>();
    auto optionsBuilder =
        std::make_unique<reader::ArrowParquetOptionsBuilder>(sharedState);
    return std::make_shared<reader::ArrowReader>(
        sharedState, std::move(optionsBuilder), std::move(fileSystem));
  }

  // Helper: create common::DataType for list<int32>
  std::shared_ptr<::common::DataType> createListInt32Type() {
    auto type = std::make_shared<::common::DataType>();
    auto* array = type->mutable_array();
    array->mutable_component_type()->set_primitive_type(
        ::common::PrimitiveType::DT_SIGNED_INT32);
    return type;
  }

  // Helper: create common::DataType for list<int64>
  std::shared_ptr<::common::DataType> createListInt64Type() {
    auto type = std::make_shared<::common::DataType>();
    auto* array = type->mutable_array();
    array->mutable_component_type()->set_primitive_type(
        ::common::PrimitiveType::DT_SIGNED_INT64);
    return type;
  }

  // Helper: create common::DataType for list<double>
  std::shared_ptr<::common::DataType> createListDoubleType() {
    auto type = std::make_shared<::common::DataType>();
    auto* array = type->mutable_array();
    array->mutable_component_type()->set_primitive_type(
        ::common::PrimitiveType::DT_DOUBLE);
    return type;
  }

  // Helper: create common::DataType for list<string>
  std::shared_ptr<::common::DataType> createListStringType() {
    auto type = std::make_shared<::common::DataType>();
    auto* array = type->mutable_array();
    array->mutable_component_type()->mutable_string()->mutable_var_char();
    return type;
  }

  // Helper: create common::DataType for list<list<int32>> (nested)
  std::shared_ptr<::common::DataType> createNestedListInt32Type() {
    auto type = std::make_shared<::common::DataType>();
    auto* array = type->mutable_array();
    auto* inner = array->mutable_component_type()->mutable_array();
    inner->mutable_component_type()->set_primitive_type(
        ::common::PrimitiveType::DT_SIGNED_INT32);
    return type;
  }

  // Helper: create common::DataType for int64
  std::shared_ptr<::common::DataType> createInt64Type() {
    auto type = std::make_shared<::common::DataType>();
    type->set_primitive_type(::common::PrimitiveType::DT_SIGNED_INT64);
    return type;
  }

  // Helper: create common::DataType for string
  std::shared_ptr<::common::DataType> createStringType() {
    auto type = std::make_shared<::common::DataType>();
    type->mutable_string()->mutable_var_char();
    return type;
  }
};

// Test 1: Single-layer list<int32>
TEST_F(ParquetListTest, TestListInt32) {
  // Build a table with a list<int32> column
  auto int32_type = arrow::int32();
  auto list_type = arrow::list(int32_type);

  auto schema = arrow::schema(
      {arrow::field("id", arrow::int64()), arrow::field("tags", list_type)});

  // Build id column
  arrow::Int64Builder id_builder;
  ASSERT_OK(id_builder.AppendValues({1, 2, 3}));
  auto id_array = id_builder.Finish().ValueOrDie();

  // Build list<int32> column: [[1,2,3], [4,5], [6]]
  auto value_builder = std::make_shared<arrow::Int32Builder>();
  arrow::ListBuilder list_builder(arrow::default_memory_pool(), value_builder);

  ASSERT_OK(list_builder.Append());
  ASSERT_OK(value_builder->AppendValues({1, 2, 3}));
  ASSERT_OK(list_builder.Append());
  ASSERT_OK(value_builder->AppendValues({4, 5}));
  ASSERT_OK(list_builder.Append());
  ASSERT_OK(value_builder->AppendValues({6}));

  auto list_array = list_builder.Finish().ValueOrDie();

  auto table = arrow::Table::Make(schema, {id_array, list_array});
  writeParquetFile("list_int32.parquet", table);

  // Read back with ArrowReader
  auto sharedState =
      createParquetSharedState("list_int32.parquet", {"id", "tags"},
                               {createInt64Type(), createListInt32Type()});
  auto reader = createParquetReader(sharedState);

  auto localState = std::make_shared<reader::ReadLocalState>();
  execution::Context ctx;
  reader->read(localState, ctx);

  EXPECT_EQ(ctx.col_num(), 2);
  EXPECT_EQ(ctx.row_num(), 3);

  // Verify the list column type
  auto col1 = ctx.columns[1];
  ASSERT_EQ(col1->column_type(), execution::ContextColumnType::kArrowArray);
  auto arrayCol =
      std::dynamic_pointer_cast<execution::ArrowArrayContextColumn>(col1);
  ASSERT_NE(arrayCol, nullptr);
  auto arrowType = arrayCol->GetArrowType();
  EXPECT_TRUE(arrowType->id() == arrow::Type::LIST)
      << "Expected list type, got: " << arrowType->ToString();

  // Verify element type is int32
  auto listType = std::static_pointer_cast<arrow::ListType>(arrowType);
  EXPECT_TRUE(listType->value_type()->Equals(arrow::int32()))
      << "Expected int32 value type, got: "
      << listType->value_type()->ToString();
}

// Test 2: Single-layer list<double>
TEST_F(ParquetListTest, TestListDouble) {
  auto list_type = arrow::list(arrow::float64());
  auto schema = arrow::schema(
      {arrow::field("id", arrow::int64()), arrow::field("scores", list_type)});

  arrow::Int64Builder id_builder;
  ASSERT_OK(id_builder.AppendValues({1, 2}));
  auto id_array = id_builder.Finish().ValueOrDie();

  auto value_builder = std::make_shared<arrow::DoubleBuilder>();
  arrow::ListBuilder list_builder(arrow::default_memory_pool(), value_builder);

  ASSERT_OK(list_builder.Append());
  ASSERT_OK(value_builder->AppendValues({1.1, 2.2, 3.3}));
  ASSERT_OK(list_builder.Append());
  ASSERT_OK(value_builder->AppendValues({4.4, 5.5}));

  auto list_array = list_builder.Finish().ValueOrDie();
  auto table = arrow::Table::Make(schema, {id_array, list_array});
  writeParquetFile("list_double.parquet", table);

  auto sharedState =
      createParquetSharedState("list_double.parquet", {"id", "scores"},
                               {createInt64Type(), createListDoubleType()});
  auto reader = createParquetReader(sharedState);

  auto localState = std::make_shared<reader::ReadLocalState>();
  execution::Context ctx;
  reader->read(localState, ctx);

  EXPECT_EQ(ctx.col_num(), 2);
  EXPECT_EQ(ctx.row_num(), 2);

  auto col1 = ctx.columns[1];
  ASSERT_EQ(col1->column_type(), execution::ContextColumnType::kArrowArray);
  auto arrayCol =
      std::dynamic_pointer_cast<execution::ArrowArrayContextColumn>(col1);
  auto arrowType = arrayCol->GetArrowType();
  EXPECT_TRUE(arrowType->id() == arrow::Type::LIST)
      << "Expected list type, got: " << arrowType->ToString();
  auto listType = std::static_pointer_cast<arrow::ListType>(arrowType);
  EXPECT_TRUE(listType->value_type()->Equals(arrow::float64()))
      << "Expected float64 value type, got: "
      << listType->value_type()->ToString();
}

// Test 3: Single-layer list<string>
TEST_F(ParquetListTest, TestListString) {
  auto list_type = arrow::list(arrow::utf8());
  auto schema = arrow::schema(
      {arrow::field("id", arrow::int64()), arrow::field("names", list_type)});

  arrow::Int64Builder id_builder;
  ASSERT_OK(id_builder.AppendValues({1, 2}));
  auto id_array = id_builder.Finish().ValueOrDie();

  auto value_builder = std::make_shared<arrow::StringBuilder>();
  arrow::ListBuilder list_builder(arrow::default_memory_pool(), value_builder);

  ASSERT_OK(list_builder.Append());
  ASSERT_OK(value_builder->Append("hello"));
  ASSERT_OK(value_builder->Append("world"));
  ASSERT_OK(list_builder.Append());
  ASSERT_OK(value_builder->Append("foo"));

  auto list_array = list_builder.Finish().ValueOrDie();
  auto table = arrow::Table::Make(schema, {id_array, list_array});
  writeParquetFile("list_string.parquet", table);

  auto sharedState =
      createParquetSharedState("list_string.parquet", {"id", "names"},
                               {createInt64Type(), createListStringType()});
  auto reader = createParquetReader(sharedState);

  auto localState = std::make_shared<reader::ReadLocalState>();
  execution::Context ctx;
  reader->read(localState, ctx);

  EXPECT_EQ(ctx.col_num(), 2);
  EXPECT_EQ(ctx.row_num(), 2);

  auto col1 = ctx.columns[1];
  ASSERT_EQ(col1->column_type(), execution::ContextColumnType::kArrowArray);
  auto arrayCol =
      std::dynamic_pointer_cast<execution::ArrowArrayContextColumn>(col1);
  auto arrowType = arrayCol->GetArrowType();
  // Parquet list<string> may come back as list<utf8> or list<large_utf8>
  EXPECT_TRUE(arrowType->id() == arrow::Type::LIST ||
              arrowType->id() == arrow::Type::LARGE_LIST)
      << "Expected list type, got: " << arrowType->ToString();
}

// Test 4: Multi-layer nested list<list<int32>>
TEST_F(ParquetListTest, TestNestedListInt32) {
  // list<list<int32>>
  auto inner_list_type = arrow::list(arrow::int32());
  auto outer_list_type = arrow::list(inner_list_type);

  auto schema = arrow::schema({arrow::field("id", arrow::int64()),
                               arrow::field("matrix", outer_list_type)});

  arrow::Int64Builder id_builder;
  ASSERT_OK(id_builder.AppendValues({1, 2}));
  auto id_array = id_builder.Finish().ValueOrDie();

  // Build nested list: [[[1,2],[3,4]], [[5,6,7]]]
  auto int32_builder = std::make_shared<arrow::Int32Builder>();
  auto inner_builder = std::make_shared<arrow::ListBuilder>(
      arrow::default_memory_pool(), int32_builder);
  arrow::ListBuilder outer_builder(arrow::default_memory_pool(), inner_builder);

  // Row 1: [[1,2],[3,4]]
  ASSERT_OK(outer_builder.Append());
  ASSERT_OK(inner_builder->Append());
  ASSERT_OK(int32_builder->AppendValues({1, 2}));
  ASSERT_OK(inner_builder->Append());
  ASSERT_OK(int32_builder->AppendValues({3, 4}));

  // Row 2: [[5,6,7]]
  ASSERT_OK(outer_builder.Append());
  ASSERT_OK(inner_builder->Append());
  ASSERT_OK(int32_builder->AppendValues({5, 6, 7}));

  auto outer_array = outer_builder.Finish().ValueOrDie();
  auto table = arrow::Table::Make(schema, {id_array, outer_array});
  writeParquetFile("nested_list.parquet", table);

  // Read back
  auto sharedState = createParquetSharedState(
      "nested_list.parquet", {"id", "matrix"},
      {createInt64Type(), createNestedListInt32Type()});
  auto reader = createParquetReader(sharedState);

  auto localState = std::make_shared<reader::ReadLocalState>();
  execution::Context ctx;
  reader->read(localState, ctx);

  EXPECT_EQ(ctx.col_num(), 2);
  EXPECT_EQ(ctx.row_num(), 2);

  // Verify nested list type
  auto col1 = ctx.columns[1];
  ASSERT_EQ(col1->column_type(), execution::ContextColumnType::kArrowArray);
  auto arrayCol =
      std::dynamic_pointer_cast<execution::ArrowArrayContextColumn>(col1);
  auto arrowType = arrayCol->GetArrowType();
  EXPECT_TRUE(arrowType->id() == arrow::Type::LIST)
      << "Expected outer list type, got: " << arrowType->ToString();

  auto outerListType = std::static_pointer_cast<arrow::ListType>(arrowType);
  EXPECT_TRUE(outerListType->value_type()->id() == arrow::Type::LIST)
      << "Expected inner list type, got: "
      << outerListType->value_type()->ToString();

  auto innerListType =
      std::static_pointer_cast<arrow::ListType>(outerListType->value_type());
  EXPECT_TRUE(innerListType->value_type()->Equals(arrow::int32()))
      << "Expected int32 leaf type, got: "
      << innerListType->value_type()->ToString();
}

// Test 5: List with null elements
TEST_F(ParquetListTest, TestListWithNulls) {
  auto list_type = arrow::list(arrow::int32());
  auto schema =
      arrow::schema({arrow::field("id", arrow::int64()),
                     arrow::field("values", list_type, /*nullable=*/true)});

  arrow::Int64Builder id_builder;
  ASSERT_OK(id_builder.AppendValues({1, 2, 3}));
  auto id_array = id_builder.Finish().ValueOrDie();

  auto value_builder = std::make_shared<arrow::Int32Builder>();
  arrow::ListBuilder list_builder(arrow::default_memory_pool(), value_builder);

  // Row 1: [1,2]
  ASSERT_OK(list_builder.Append());
  ASSERT_OK(value_builder->AppendValues({1, 2}));
  // Row 2: null
  ASSERT_OK(list_builder.AppendNull());
  // Row 3: [3]
  ASSERT_OK(list_builder.Append());
  ASSERT_OK(value_builder->AppendValues({3}));

  auto list_array = list_builder.Finish().ValueOrDie();
  auto table = arrow::Table::Make(schema, {id_array, list_array});
  writeParquetFile("list_nulls.parquet", table);

  auto sharedState =
      createParquetSharedState("list_nulls.parquet", {"id", "values"},
                               {createInt64Type(), createListInt32Type()});
  auto reader = createParquetReader(sharedState);

  auto localState = std::make_shared<reader::ReadLocalState>();
  execution::Context ctx;
  reader->read(localState, ctx);

  EXPECT_EQ(ctx.col_num(), 2);
  EXPECT_EQ(ctx.row_num(), 3);

  auto col1 = ctx.columns[1];
  ASSERT_EQ(col1->column_type(), execution::ContextColumnType::kArrowArray);
  auto arrayCol =
      std::dynamic_pointer_cast<execution::ArrowArrayContextColumn>(col1);
  auto arrowType = arrayCol->GetArrowType();
  EXPECT_TRUE(arrowType->id() == arrow::Type::LIST)
      << "Expected list type, got: " << arrowType->ToString();
}

// Test 6: Fixed-size list
TEST_F(ParquetListTest, TestFixedSizeList) {
  // fixed_size_list<int32>[3]
  auto fsl_type = arrow::fixed_size_list(arrow::int32(), 3);
  auto schema = arrow::schema(
      {arrow::field("id", arrow::int64()), arrow::field("vec", fsl_type)});

  arrow::Int64Builder id_builder;
  ASSERT_OK(id_builder.AppendValues({1, 2}));
  auto id_array = id_builder.Finish().ValueOrDie();

  auto value_builder = std::make_shared<arrow::Int32Builder>();
  arrow::FixedSizeListBuilder fsl_builder(arrow::default_memory_pool(),
                                          value_builder, 3);

  // Row 1: [10, 20, 30]
  ASSERT_OK(fsl_builder.Append());
  ASSERT_OK(value_builder->AppendValues({10, 20, 30}));
  // Row 2: [40, 50, 60]
  ASSERT_OK(fsl_builder.Append());
  ASSERT_OK(value_builder->AppendValues({40, 50, 60}));

  auto fsl_array = fsl_builder.Finish().ValueOrDie();
  auto table = arrow::Table::Make(schema, {id_array, fsl_array});
  writeParquetFile("fixed_size_list.parquet", table);

  // Create type with max_length=3
  auto fixedListType = std::make_shared<::common::DataType>();
  auto* arr = fixedListType->mutable_array();
  arr->mutable_component_type()->set_primitive_type(
      ::common::PrimitiveType::DT_SIGNED_INT32);
  arr->set_max_length(3);

  auto sharedState =
      createParquetSharedState("fixed_size_list.parquet", {"id", "vec"},
                               {createInt64Type(), fixedListType});
  auto reader = createParquetReader(sharedState);

  auto localState = std::make_shared<reader::ReadLocalState>();
  execution::Context ctx;
  reader->read(localState, ctx);

  EXPECT_EQ(ctx.col_num(), 2);
  EXPECT_EQ(ctx.row_num(), 2);

  auto col1 = ctx.columns[1];
  ASSERT_EQ(col1->column_type(), execution::ContextColumnType::kArrowArray);
  auto arrayCol =
      std::dynamic_pointer_cast<execution::ArrowArrayContextColumn>(col1);
  auto arrowType = arrayCol->GetArrowType();
  // Parquet may read fixed_size_list back as regular list
  // depending on the Parquet writer and reader implementation
  EXPECT_TRUE(arrowType->id() == arrow::Type::LIST ||
              arrowType->id() == arrow::Type::FIXED_SIZE_LIST)
      << "Expected list or fixed_size_list type, got: "
      << arrowType->ToString();
}

// Test 7: Schema inference for Parquet with list columns
TEST_F(ParquetListTest, TestSchemaInference) {
  auto list_type = arrow::list(arrow::int64());
  auto schema = arrow::schema(
      {arrow::field("id", arrow::int64()), arrow::field("items", list_type)});

  arrow::Int64Builder id_builder;
  ASSERT_OK(id_builder.AppendValues({1}));
  auto id_array = id_builder.Finish().ValueOrDie();

  auto value_builder = std::make_shared<arrow::Int64Builder>();
  arrow::ListBuilder list_builder(arrow::default_memory_pool(), value_builder);
  ASSERT_OK(list_builder.Append());
  ASSERT_OK(value_builder->AppendValues({10, 20}));
  auto list_array = list_builder.Finish().ValueOrDie();

  auto table = arrow::Table::Make(schema, {id_array, list_array});
  writeParquetFile("infer_list.parquet", table);

  // Create shared state without specifying entry schema types (for inference)
  auto sharedState =
      createParquetSharedState("infer_list.parquet", {"id", "items"},
                               {createInt64Type(), createListInt64Type()});
  auto reader = createParquetReader(sharedState);

  // Infer schema
  auto inferResult = reader->inferSchema();
  ASSERT_TRUE(inferResult.ok()) << inferResult.status().ToString();

  auto inferredSchema = inferResult.ValueOrDie();
  ASSERT_EQ(inferredSchema->num_fields(), 2);

  // Verify inferred types
  EXPECT_TRUE(inferredSchema->field(0)->type()->Equals(arrow::int64()));

  auto itemsType = inferredSchema->field(1)->type();
  EXPECT_TRUE(itemsType->id() == arrow::Type::LIST)
      << "Expected list type from inference, got: " << itemsType->ToString();
}

// Test 8: Type converter round-trip for list types
TEST_F(ParquetListTest, TestTypeConverterRoundTrip) {
  reader::ArrowTypeConverter converter;

  // list<int32> round-trip
  {
    auto commonType = createListInt32Type();
    auto arrowType = converter.convert(*commonType);
    ASSERT_NE(arrowType, nullptr);
    EXPECT_TRUE(arrowType->Equals(arrow::list(arrow::int32())));

    auto backToCommon = converter.convert(*arrowType);
    ASSERT_NE(backToCommon, nullptr);
    EXPECT_TRUE(backToCommon->has_array());
    EXPECT_EQ(backToCommon->array().component_type().primitive_type(),
              ::common::PrimitiveType::DT_SIGNED_INT32);
  }

  // list<list<int32>> round-trip
  {
    auto commonType = createNestedListInt32Type();
    auto arrowType = converter.convert(*commonType);
    ASSERT_NE(arrowType, nullptr);
    EXPECT_TRUE(arrowType->Equals(arrow::list(arrow::list(arrow::int32()))));

    auto backToCommon = converter.convert(*arrowType);
    ASSERT_NE(backToCommon, nullptr);
    EXPECT_TRUE(backToCommon->has_array());
    EXPECT_TRUE(backToCommon->array().component_type().has_array());
    EXPECT_EQ(backToCommon->array()
                  .component_type()
                  .array()
                  .component_type()
                  .primitive_type(),
              ::common::PrimitiveType::DT_SIGNED_INT32);
  }
}

}  // namespace test
}  // namespace neug
