/**
 * Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once
#include "neug/compiler/function/function.h"
namespace neug {
namespace function {
struct ProjectGraphFunction {
  static constexpr const char* name = "project_graph";
  static function_set getFunctionSet();
};
struct DropProjectedGraphFunction {
  static constexpr const char* name = "drop_projected_graph";
  static function_set getFunctionSet();
};
}  // namespace function
}  // namespace neug