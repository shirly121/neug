## 1. Schema and codegen

- [ ] 1.1 Add `ToList` message and `to_list` field to `proto/expr.proto`
- [ ] 1.2 Regenerate C++ and Python protobuf stubs

## 2. Compiler

- [ ] 2.1 Branch `ListCreation` in `GExprConverter`: `LIST` → `ToList`, `STRUCT` → `ToTuple`

## 3. Execution

- [ ] 3.1 Add `ListExpr` / bind path producing `Value::LIST`
- [ ] 3.2 Handle `kToList` in `expr.cc` (build_expr + shunting-yard)

## 4. Tests

- [ ] 4.1 Add or adjust tests for homogeneous vs heterogeneous list literals
