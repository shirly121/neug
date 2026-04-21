## Context

- [`specs/004-gds/compiler-spec2.md`](../../../specs/004-gds/compiler-spec2.md) defines the user-facing example **`CALL k_core('my_graph', {min_k: 3, concurrency: 4}) YIELD node, core_number;`** and the intended compiler artifacts: **`GDSAlgo`** protobuf, **`GDSFuncBindData`**, **`GDSAlgoFunction`**, and **`GQueryConvertor`** lowering via **`convertGDSFunction`**.
- Today **`GQueryConvertor::convertTableFunc`** only distinguishes **`ScanFileBindData`** vs everything else (which becomes **`convertProcedureCall`**). There is **no** **`GDSAlgo`** message in **`proto/`** yet (search as of this change).
- **`NeugCallFunction`** ([`neug_call_function.h`](../../../include/neug/compiler/function/neug_call_function.h)) carries **`call_bind_func_t` / `call_exec_func_t`** for execution; **`TableFunction::bindFunc`** (see **`TableFunction`** / binder) is the compiler-time hook used for file scan and, per **`gds-project-drop-graph`**, for mutating **`GraphEntrySet`** on **`project_graph`**. GDS algorithm **`CALL`** should use the same **`TableFunction::bindFunc`** pattern to build **`GDSFuncBindData`** if not already covered by default binder behavior.
- Session graph entries (**`graph::GraphEntry` / `ParsedGraphEntry`**, **`GraphEntrySet`**) are the source of truth for the **first** argument (projected graph name) once **`project_graph`** is available.

## Goals / Non-Goals

**Goals:**

- Emit a **physical operator** whose payload is **`GDSAlgo`** (or a thin wrapper in **`physical.proto`**) so the runtime can schedule GDS without treating the call as an opaque **`ProcedureCall`**.
- **Bind time:** Resolve **`graph_name`** to a **`graph::GraphEntry`** (via **`bindGraphEntry`** or **`GraphEntrySet::getEntry`** + validation), **schema-bind** vertex/edge predicates into **`common::Expression`**, normalize **options** into **`map<string, string>`** (or the proto’s chosen representation) from the literal **`MAP`** second argument.
- **Per-algorithm registration:** Each supported algorithm is a **`GDSAlgoFunction`** instance (name, input type list, **`YIELD`** column list) with an **`algo_exec_func_t`** hook for execution (may be stubbed initially).
- Implement **`convertGDSFunction`** as specified: **`algo_name`** from the **`CALL`** function name, **`sub_graph`** / **`options`** from **`GDSFuncBindData`**.

**Non-Goals:**

- Full **execution** of **`k_core`** (or other algos) inside this compiler change—only correct plan shape and stubs where the codebase requires non-null **`execFunc`**.
- Defining the complete catalog of GDS algorithms; start with **`k_core`** (spec example) and document how to add more.

## Decisions

1. **Protobuf placement**  
   **Decision:** Add **`Subgraph`** / **`GDSAlgo`** to **`proto/physical.proto`** (or a small included **`proto/gds.proto`** if the repo prefers splitting), and add a **`PhysicalOpr`** variant (e.g. **`gds_algo`**) alongside **`procedure_call`**.  
   **Rationale:** Keeps the physical plan self-contained for the executor; matches the sketch in **`compiler-spec2.md`**.  
   **Alternative:** Serialize **`GDSAlgo`** inside **`ProcedureCall`**—rejected to avoid overloading procedure semantics and to keep GDS-specific fields typed.

2. **Bind data type**  
   **Decision:** Introduce **`function::gds::GDSFuncBindData`** : **`TableFuncBindData`** holding **`graph::GraphEntry`** (or **`std::shared_ptr<const graph::GraphEntry>`** if that matches existing ownership), **`options_t`** (**`case_insensitive_map_t<std::string>`**), and any precomputed **schema-bound subgraph** needed to fill **`Subgraph`** without re-walking binder state in the converter.  
   **Rationale:** Mirrors the spec; gives **`convertGDSFunction`** a single source of truth.

3. **Function object**  
   **Decision:** **`GDSAlgoFunction`** extends **`NeugCallFunction`**, sets **`TableFunction::bindFunc`** to **`bindGDSFunction`**, and stores **`algo_exec_func_t execFunc`** for the executor.  
   **Rationale:** Aligns with the pseudocode in **`compiler-spec2.md`**; consistent with other **`NeugCallFunction`** uses.

4. **Converter dispatch**  
   **Decision:** In **`convertTableFunc`**, after **`ScanFileBindData`**, test **`dynamic_cast<const gds::GDSFuncBindData*>`** and call **`convertGDSFunction`**; else **`convertProcedureCall`**.  
   **Rationale:** Exact structure from **`compiler-spec2.md`**; minimal disruption to existing procedures.

5. **Options map typing**  
   **Decision:** At bind time, coerce supported option values (int, bool, string) to **string** for **`map<string, string>`** in protobuf; reject unsupported **`Value`** kinds with a clear binder error.  
   **Rationale:** Proto sketch uses **`map<string, string>`**; avoids protobuf **`Value`** oneof explosion in v1.

6. **Naming bug in spec snippet**  
   **Note:** **`compiler-spec2.md`** labels the converter helper as **`convertDataSource`** in one block; implementation SHALL be **`convertGDSFunction`** (as in the surrounding **`convertTableFunc`** text).

## Risks / Trade-offs

- **[Risk] Proto / codegen churn** — Adding **`PhysicalOpr`** fields touches many switch sites. **Mitigation:** Follow existing patterns for **`ProcedureCall`**; add default “unsupported” handling only where required.
- **[Risk] Subgraph binding complexity** — Mapping **`ParsedGraphEntry`** string predicates to **schema-bound `Expression`** may require binder utilities not yet shared. **Mitigation:** Phase 1 may bind empty predicates where allowed; document gaps in **`tasks.md`**.
- **[Risk] Duplicate work with `gds-project-drop-graph`** — Graph registration and **`TableFunction::bindFunc`** wiring may overlap. **Mitigation:** Reuse the same binder hook; do not fork **`GraphEntrySet`** logic.

## Migration Plan

- **Additive:** New physical op and new functions; existing **`CALL`** procedures unchanged.
- **Tests:** Compiler tests that bind **`CALL k_core(...)`** (or mocked algo) and assert physical plan contains **`GDSAlgo`** with expected **`algo_name`** and options keys.

## Open Questions

- Exact **`PhysicalOpr`** field number and naming convention with the team’s proto style guide.
- Whether **`YIELD`** columns are fixed per algorithm or derived from a central metadata table.
- Final list of v1 algorithms beyond **`k_core`**.
