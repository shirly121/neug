## Context

`ListCreationFunction` binding already yields `LogicalType::LIST` when all elements share a type and `LogicalType::STRUCT` when they differ (`list_creation.cpp`). GExprConverter ignored that and always emitted protobuf `ToTuple`. Execution built `TupleExpr`, which types results as struct, not list.

## Goals / Non-Goals

**Goals:**

- Encode homogeneous list literals as `ExprOpr.to_list` / `ToList` with the same field layout as `ToTuple`.
- Encode heterogeneous list literals as `ToTuple` unchanged.
- Evaluate `ToList` into runtime list `Value` with correct list element type from IR `node_type`.

**Non-Goals:**

- Changing `GroupBy` aggregate `TO_LIST` (aggregation collect).
- Changing binder list/tuple typing rules.

## Decisions

- **Proto**: Add `message ToList { repeated Expression fields = 1; }` and `ToList to_list = 24` in `ExprOpr` (next free field after `scalar_func = 23`).
- **Converter**: Branch on `expr.getDataType().getLogicalTypeID()`: `LIST` → `ToList`, `STRUCT` → `ToTuple`. Reuse child conversion loop; set `node_type` from `typeConverter.convertLogicalType(expr.getDataType())`.
- **Runtime**: New `ListExpr` + `BindedListExpr` mirroring tuple structure but calling `Value::LIST` and using list `DataType` from IR `node_type` / children.
- **Expr parse**: Handle `kToList` wherever `kToTuple` is handled in shunting-yard and `build_expr`.

## Risks / Trade-offs

- **Legacy plans** only containing `ToTuple` → unchanged behavior until replanned.
- **ANY-heavy list literals** → binder may still yield `LIST`; execution must accept `parse_from_ir_data_type` for list child type.

## Migration Plan

Deploy compiler + runtime together; no on-disk plan migration required if plans are generated per query.

## Open Questions

- None for initial implementation.
