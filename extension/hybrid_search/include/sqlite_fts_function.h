#pragma once

#include "neug/common/types/value.h"
#include "neug/compiler/function/function.h"

namespace neug::sqlite_fts_ext {

struct SQLiteFTSBM25Function {
  static constexpr const char* name = "BM25";

  static function::function_set getFunctionSet();
  static Value Exec(const std::vector<Value>& args);
};

}  // namespace neug::sqlite_fts_ext
