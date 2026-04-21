## 1. Protobuf and codegen

- [ ] 1.1 Add **`Subgraph`** (vertex/edge entries with label ids + **`common::Expression`** predicates) and **`GDSAlgo`** (**`algo_name`**, **`sub_graph`**, **`options`**) per [`specs/004-gds/compiler-spec2.md`](../../../specs/004-gds/compiler-spec2.md); wire into **`physical.proto`** (new **`PhysicalOpr`** branch or equivalent).
- [ ] 1.2 Regenerate C++ protobuf targets and fix compile breaks from new enum/oneof dispatch sites (follow **`ProcedureCall`** usage as a template).

## 2. Bind data and GDS call function type

- [ ] 2.1 Add **`GDSFuncBindData`** (extends **`TableFuncBindData`**) with **`graph::GraphEntry`** (or approved handle), **`options_t`**, and fields needed to build **`Subgraph`** in the converter.
- [ ] 2.2 Implement **`bindGDSFunction(main::ClientContext*, const TableFuncBindInput*)`** validating: graph name resolves in **`GraphEntrySet`**, second arg is a literal map, options keys are known/sane for the target algo.
- [ ] 2.3 Add **`GDSAlgoFunction`** (**`NeugCallFunction`** subclass) setting **`TableFunction::bindFunc`**, **`algo_exec_func_t execFunc`**, and per-algorithm **`YIELD`** metadata.

## 3. Schema binding for subgraph

- [ ] 3.1 From **`GraphEntry`** / **`ParsedGraphEntry`**, produce **`pb::Subgraph`** with **schema-bound** label ids and **`Expression`** predicates (reuse binder/schema helpers; stub only if explicitly agreed).
- [ ] 3.2 Ensure predicates reference properties that exist in the current **`Schema`** version (per spec comment).

## 4. Catalog registration

- [ ] 4.1 Register at least **`k_core`** (name aligned with Cypher **`CALL k_core`**) with signature **`(STRING, MAP)`** or the project’s canonical types; add **`TABLE_FUNCTION`** / **`getFunctionSet`** wiring in **`function_collection.cpp`** (and **`CMakeLists`** if new `.cpp`).

## 5. Physical conversion

- [ ] 5.1 Implement **`GQueryConvertor::convertGDSFunction`**: populate **`GDSAlgo`** from **`GDSFuncBindData`** and append **`PhysicalOpr`**.
- [ ] 5.2 Update **`convertTableFunc`** to dispatch **`GDSFuncBindData`** before **`convertProcedureCall`** ([`g_query_converter.cpp`](../../../src/compiler/gopt/g_query_converter.cpp)).

## 6. Execution stubs

- [ ] 6.1 Provide minimal **`NeugCallFunction::bindFunc` / `execFunc`** for **`GDSAlgoFunction`** consistent with other table **`CALL`** functions (empty result or framework-required columns) until engine consumes **`GDSAlgo`**.

## 7. Verification

- [ ] 7.1 Add a compiler/planner test (or extend an existing harness) that parses/binds **`CALL k_core('g', {min_k: 3}) YIELD ...`** with a seeded **`GraphEntrySet`** entry **`g`**, and asserts the physical plan includes **`GDSAlgo`** with **`algo_name`** **`k_core`** and expected string options.
- [ ] 7.2 Build affected targets and fix warnings/errors.
