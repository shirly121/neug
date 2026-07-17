#pragma once

#include "neug/common/types/value.h"
#include "neug/compiler/function/function.h"

namespace neug::zvec_ext {

struct VectorDistanceL2Function {
  static constexpr const char* name = "VECTOR_DISTANCE_L2";

  static function::function_set getFunctionSet();
  static Value Exec(const std::vector<Value>& args);
};

struct VectorDistanceCosineFunction {
  static constexpr const char* name = "VECTOR_DISTANCE_COSINE";

  static function::function_set getFunctionSet();
  static Value Exec(const std::vector<Value>& args);
};

struct VectorDistanceIPFunction {
  static constexpr const char* name = "VECTOR_DISTANCE_IP";

  static function::function_set getFunctionSet();
  static Value Exec(const std::vector<Value>& args);
};

}  // namespace neug::zvec_ext
