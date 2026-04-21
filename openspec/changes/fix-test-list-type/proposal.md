## Why

[`tests/test_ddl.py::test_list_type`](tools/python_bind/tests/test_ddl.py) exercises **DDL + DML** for `STRING[]` on vertices and edges (`tags`, `values`) and **read-back** via `MATCH ... RETURN`. Today the engine fails when building **reference views** over stored list columns: [`CreateRefColumn`](src/utils/property/column.cc) handles scalars and `kVarchar` but **not `DataTypeId::kList`**, producing `Not supported: Unsupported type for reference column` (and downstream “Unknown” in some paths). That blocks list-typed properties in the property-graph storage layer used by queries.

## What Changes

- Extend **`CreateRefColumn`** (and supporting types in [`column.h`](include/neug/utils/property/column.h) / [`column.cc`](src/utils/property/column.cc)) to support columns whose storage is **`ListColumn`** (`kList`), returning list **`Property`** values consistent with `ListColumn::get_prop`.
- Fix or narrow any **schema YAML** / type-id warnings tied to **LIST** serialization if they affect durability or planner reload (e.g. `g_type_utils.h` “Unsupported type in YAML” for list metadata).
- **Unskip** (or keep enabled) **`test_list_type`** and ensure **`tools/python_bind`** pytest passes for node + edge list round-trip.

## Capabilities

### New Capabilities

- `list-ref-column-support`: Reference-column API over `ListColumn` so scan/project paths can read list-typed vertex and edge properties.

### Modified Capabilities

- (none — no prior openspec spec folder for storage; behavior is new coverage.)

## Impact

- `src/utils/property/column.cc`, `include/neug/utils/property/column.h`
- Call sites of `CreateRefColumn` (unchanged signature expected)
- Optional: `g_type_utils.h` / graph YAML dump for LIST types
- `tools/python_bind/tests/test_ddl.py` — test must pass without skip
