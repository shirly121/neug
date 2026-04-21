## 1. GraphEntrySet and helpers

- [ ] 1.1 Add **`validateGraphNotExist`** / **`validateGraphExist`** on **`graph::GraphEntrySet`** (or adjacent helpers) using **`hasGraph`**, with exceptions consistent with binder/runtime style in this codebase.
- [ ] 1.2 Confirm **`ParsedGraphEntry`** / **`GraphEntryTableInfo`** population matches [`design.md`](design.md) (no alternate “native” entry type).

## 2. Binder: TableFunction::bindFunc for CALL

- [ ] 2.1 Extend **`Binder::bindTableFunc`** ([`bind_table_function.cpp`](../../../src/compiler/binder/bind/bind_table_function.cpp)) to build **`TableFuncBindInput`** (positional **`params`**, **`yieldVariables`**) and invoke **`TableFunction::bindFunc`** when set; use returned **`TableFuncBindData`** when non-null.
- [ ] 2.2 Verify existing **`NeugCallFunction`** instances without **`TableFunction::bindFunc`** behave unchanged.

## 3. GDS call functions (project + drop)

- [ ] 3.1 Add **`project_graph_function`** (header + cpp) under **`src/compiler/function/gds/``: **`NeugCallFunction`** with signature **`(STRING, ANY, ANY)`**, **`TableFunction::bindFunc`** mutating **`GraphEntrySet`**, stub **`GDSFunction::bindGraphEntry`**, minimal **`NeugCallFunction::bindFunc` / `execFunc`**.
- [ ] 3.2 Implement **`extractGraphEntryTableInfos(const common::Value&)`** (or equivalent) for map/list literals; document supported **`Value`** shapes; stub unsupported branches per [`specs/004-gds/compiler-spec.md`](../../../specs/004-gds/compiler-spec.md).
- [ ] 3.3 Add **`drop_projected_graph_function`**: **`(STRING)`**, **`TableFunction::bindFunc`** calling **`validateGraphExist`** + **`dropGraph`**.
- [ ] 3.4 Wire **`gds/CMakeLists.txt`** for new sources.

## 4. Catalog registration

- [ ] 4.1 Register **`TABLE_FUNCTION(ProjectGraph...)`** and **`TABLE_FUNCTION(DropProjectedGraph...)`** in [`function_collection.cpp`](../../../src/compiler/function/function_collection.cpp) (and includes).
- [ ] 4.2 Align public **`name`** constants with Cypher (**`project_graph`**, **`drop_projected_graph`**) and existing lowercase convention.

## 5. Verification

- [ ] 5.1 Add or extend a compiler test (or smallest harness under existing tests) that prepares **`ClientContext`**, issues **`CALL`** strings or binder API, and asserts **`GraphEntrySet`** contents / errors for success, duplicate **`project_graph`,** and missing **`drop_projected_graph`**.
- [ ] 5.2 Build target **`gds_function`** / full compiler library and fix compile errors.
