## Why

Physical/IR expressions always encoded list literals (`ListCreationFunction`) as `ToTuple`, while the binder already classifies homogeneous element types as `LIST` and mixed types as `STRUCT`. This mismatch obscures the physical plan and forces execution to build tuple/struct semantics for true lists.

## What Changes

- Add `ToList` to `expr.proto` (parallel to `ToTuple`) and emit it from GExprConverter when the bound type is `LIST`.
- Keep `ToTuple` for `STRUCT` (heterogeneous list literals).
- Add execution support (`ListExpr` + `kToList` parsing) so homogeneous lists evaluate to list values with correct element types.
- Regenerate protobuf stubs (C++ and Python bind).

## Capabilities

### New Capabilities

- `physical-expr-list-vs-tuple`: IR and runtime distinguish homogeneous list construction (`to_list`) from heterogeneous tuple construction (`to_tuple`) in embedded `common.Expression` (e.g. Project, Insert property mappings).

### Modified Capabilities

- (none — no prior specs in `openspec/specs/`)

## Impact

- `proto/expr.proto`, generated `expr.pb.h` / Python `expr_pb2.py`
- `src/compiler/gopt/g_expr_converter.cpp`, `include/neug/compiler/gopt/g_expr_converter.h`
- `src/execution/expression/expr.cc`, new list expression class under `include/neug/execution/expression/exprs/`
- Tests: C++ or Python bind as appropriate
