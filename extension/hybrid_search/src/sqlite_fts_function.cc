#include "sqlite_fts_function.h"

#include <memory>
#include <vector>

#include "neug/common/extra_type_info.h"
#include "neug/compiler/function/neug_scalar_function.h"
#include "neug/utils/exception/exception.h"

namespace neug::sqlite_fts_ext {
namespace {

std::unique_ptr<function::FunctionBindData> BindBM25Array(
    const function::ScalarBindFuncInput& input) {
  if (input.arguments.size() != 2) {
    THROW_BINDER_EXCEPTION("BM25 requires two arguments");
  }
  const auto& columns_type = input.arguments[0]->getDataType();
  if (columns_type.id() != DataTypeId::kArray ||
      ArrayType::GetChildType(columns_type).id() != DataTypeId::kVarchar) {
    THROW_BINDER_EXCEPTION("BM25 column list must contain STRING properties");
  }
  std::vector<DataType> parameter_types;
  parameter_types.push_back(columns_type.copy());
  parameter_types.emplace_back(DataTypeId::kVarchar);
  return std::make_unique<function::FunctionBindData>(
      std::move(parameter_types), DataType::DOUBLE);
}

}  // namespace

function::function_set SQLiteFTSBM25Function::getFunctionSet() {
  function::function_set functions;
  auto array_function = std::make_unique<function::NeugScalarFunction>(
      name, std::vector<DataTypeId>{DataTypeId::kArray, DataTypeId::kVarchar},
      DataTypeId::kDouble, Exec);
  array_function->bindFunc = BindBM25Array;
  functions.push_back(std::move(array_function));
  functions.push_back(std::make_unique<function::NeugScalarFunction>(
      name, std::vector<DataTypeId>{DataTypeId::kVarchar, DataTypeId::kVarchar},
      DataTypeId::kDouble, Exec));
  return functions;
}

Value SQLiteFTSBM25Function::Exec(const std::vector<Value>&) {
  THROW_NOT_SUPPORTED_EXCEPTION(
      "BM25 is only supported with ORDER BY score ASC and LIMIT");
}

}  // namespace neug::sqlite_fts_ext
