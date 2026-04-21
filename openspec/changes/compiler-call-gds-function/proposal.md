## Why

Graph Data Science (GDS) algorithms are invoked from Cypher as **`CALL algo_name('projected_graph', { ... options ... }) YIELD ...`**. The compiler must recognize these calls, bind the graph name against session **`GraphEntrySet`** (projected graphs from **`project_graph`**), validate and schema-bind subgraph predicates and options, and emit a dedicated physical operator carrying a **`GDSAlgo`**-shaped plan payload—not a generic **`ProcedureCall`**. This is specified in [`specs/004-gds/compiler-spec2.md`](../../../specs/004-gds/compiler-spec2.md).

## What Changes

- Introduce (or extend) **protobuf** messages **`Subgraph`** / **`GDSAlgo`** as in **`compiler-spec2.md`**: algorithm name, schema-bound subgraph (vertex/edge entries with label ids and bound **`Expression`** predicates), and string-keyed options map.
- Add compiler types under **`include/neug/compiler/function/gds/`** (e.g. **`GDSFuncBindData`**, **`GDSAlgoFunction`** extending **`NeugCallFunction`**) with **`bindGDSFunction`** that resolves **`graph::GraphEntry`** via existing **`graph::GDSFunction::bindGraphEntry`** (or equivalent), captures **`options_t`**, and produces bind data consumed by the planner/converter.
- Register built-in GDS algorithm **`CALL`** functions (starting with algorithms required by product, e.g. **`k_core`** as in the spec example) with correct **`YIELD`** column metadata and parameter types **`(STRING, MAP)`** (or the project’s canonical map type id).
- Extend **`GQueryConvertor::convertTableFunc`** to branch on **`GDSFuncBindData`** and implement **`convertGDSFunction`**: map function name → **`algo_name`**, map bind data → **`sub_graph`** + **`options`**, append the appropriate **`PhysicalOpr`** (new oneof field or dedicated message, aligned with **`physical.proto`** evolution).

## Capabilities

### New Capabilities

- **`compiler-gds-call-algo`**: Compiler binding and physical-plan lowering for **`CALL <gds_algo>(graph_name, options_map) YIELD ...`** into a **`GDSAlgo`** physical operator.

### Modified Capabilities

- (none in `openspec/specs/` today — delta lives under this change’s `specs/`.)

## Impact

- **Primary:** `proto/physical.proto` (or adjacent proto) for **`GDSAlgo`** embedding; `include/neug/compiler/function/gds/`; `src/compiler/function/gds/`; `src/compiler/gopt/g_query_converter.cpp` / `g_query_converter.h`; built-in function registration (`function_collection.cpp` and related).
- **Secondary:** Binder / **`TableFunction::bindFunc`** path if GDS binding must run at the same hook as **`project_graph`** (reuse decisions from [`openspec/changes/gds-project-drop-graph/design.md`](../gds-project-drop-graph/design.md) where applicable).
- **Execution:** **`NeugCallFunction::bindFunc` / `execFunc`** stubs or minimal implementations per existing **`CALL`** patterns until the engine consumes **`GDSAlgo`**.
