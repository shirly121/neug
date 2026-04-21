## Context

- [`specs/004-gds/compiler-spec.md`](../../../specs/004-gds/compiler-spec.md) defines the Cypher surface (`CALL project_graph(...)`, drop) and the intent: **compiler-only** registration of projected graph metadata in **`graph::GraphEntrySet`** on **`ClientContext`** (no storage materialization).
- Today, **`graph::GraphEntrySet`** ([`include/neug/compiler/graph/graph_entry.h`](../../../include/neug/compiler/graph/graph_entry.h)) stores **`ParsedGraphEntry`** (vectors of **`GraphEntryTableInfo`**: `tableName` + string `predicate`). It exposes **`hasGraph`**, **`addGraph(name, const ParsedGraphEntry&)`**, **`dropGraph`**, **`getEntry`**. It does **not** yet expose `validateGraphExist` / `validateGraphNotExist` from the spec sketch; those can be thin helpers (same header/cpp or call sites).
- **`NeugCallFunction`** extends **`TableFunction`** ([`neug_call_function.h`](../../../include/neug/compiler/function/neug_call_function.h)): execution uses **`NeugCallFunction::bindFunc`** / **`execFunc`** (physical plan). **`TableFunction::bindFunc`** (`table_func_bind_t`) is the hook used elsewhere (e.g. file scan) for **compiler-time** binding with **`TableFuncBindInput`**.
- **`Binder::bindTableFunc`** ([`bind_table_function.cpp`](../../../src/compiler/binder/bind/bind_table_function.cpp)) matches **`NeugCallFunction`**, folds literals, and builds **`TableFuncBindData`** — it currently **does not** invoke **`TableFunction::bindFunc`**. **`ShowLoadedExtensionsFunction`** only sets **`NeugCallFunction::bindFunc` / `execFunc`**, not **`TableFunction::bindFunc`**.

## Goals / Non-Goals

**Goals:**

- Register **`project_graph(graphName, nodeMap, relMap)`** and **`drop_projected_graph(graphName)`** as **`TABLE_FUNCTION_ENTRY`** **`NeugCallFunction`** instances (same pattern as **`show_loaded_extensions`**), with names following existing **lowercase** conventions (e.g. `project_graph`, `drop_projected_graph`).
- On successful bind of **`project_graph`**, populate **`GraphEntrySet`** with a **`ParsedGraphEntry`** derived from literal map arguments (string keys → table identifiers, string values → predicate strings).
- On successful bind of **`drop_projected_graph`**, remove the named entry; **error** if the name is missing.
- On **`project_graph`**, **error** if the graph name already exists (**duplicate**).
- Invoke optional **validation** hook for graph entry shape (stub **`GDSFunction::bindGraphEntry`** or equivalent) as allowed by [`compiler-spec.md`](../../../specs/004-gds/compiler-spec.md); full semantics can remain TODO.
- Keep changes **under `src/compiler/`** (and includes under `include/neug/compiler/` as needed); document any unavoidable touch points.

**Non-Goals:**

- Physical graph construction, engine execution of predicates, or persistence beyond the session **`GraphEntrySet`**.
- Completing end-to-end GDS algorithms or planner consumption of projected graph names (may be covered by [`specs/004-gds/spec.md`](../../../specs/004-gds/spec.md) separately).

## Decisions

1. **Where compiler-time side effects run**  
   **Decision:** Extend **`Binder::bindTableFunc`** so that after positional parameters are bound (and folded), if the resolved **`TableFunction::bindFunc`** is non-null, the binder builds a **`TableFuncBindInput`** (populate **`params`** from the positional **`expression_vector`**, optional **`yieldVariables`**) and calls **`tableFunc->bindFunc(clientContext, &bindInput)`**. If it returns non-null **`TableFuncBindData`**, use that as the clause bind data; otherwise keep the current default **`TableFuncBindData`** construction.  
   **Rationale:** Today only file-scan style paths call **`TableFunction::bindFunc`**; **`CALL`** procedures need the same hook for **`GraphEntrySet`** mutation without involving the execution **`NeugCallFunction::bindFunc`**.  
   **Alternative considered:** Hard-code graph registration inside **`bindInQueryCall`** for specific function names — rejected as brittle and harder to test in isolation.

2. **Naming and signatures**  
   **Decision:** Cypher-visible names: **`project_graph`**, **`drop_projected_graph`**. Signature: **`(STRING, ANY, ANY)`** for project (two map-like literals), **`(STRING)`** for drop; align **`parameterTypeIDs`** with **`BuiltInFunctionsUtils::matchFunction`** and existing **`ANY`** handling for nested map/list literals.

3. **Data model alignment**  
   **Decision:** Use **`ParsedGraphEntry`** / **`GraphEntryTableInfo`** only — not **`ParsedNativeGraphEntry`**. **`addGraph`** copies by value; build a **`ParsedGraphEntry`** local and pass **`const ParsedGraphEntry&`**.

4. **Validation helpers on `GraphEntrySet`**  
   **Decision:** Add **`validateGraphNotExist` / `validateGraphExist`** as small methods (or free functions in `graph_entry.cpp`) that throw **`Binder`** or **`Runtime`** exceptions consistent with nearby code when **`hasGraph`** disagrees with the operation.

5. **Extracting map literals**  
   **Decision:** Implement **`extractGraphEntryTableInfos(const common::Value&)`** in a gds helper under **`src/compiler/function/gds/`**: walk **`Value`** for **`MAP`/`STRUCT`** (whatever the binder produces for `{...}`) into a list of **`GraphEntryTableInfo`**. If structure differs by literal form, document the supported **`Value`** shape and return clear errors for unsupported forms. Per product note, **stub** branches may defer rare cases with `NEUG_NOT_IMPLEMENTED` or empty vectors where explicitly acceptable.

6. **`GDSFunction::bindGraphEntry`**  
   **Decision:** Introduce a minimal namespace (e.g. **`neug::function::GDSFunction`**) with **`bindGraphEntry(ClientContext&, const ParsedGraphEntry&)`** that either no-ops or performs shallow checks; expand later when catalog binding exists.

7. **Engine `NeugCallFunction::bindFunc` / `execFunc`**  
   **Decision:** Match **`show_loaded_extensions`** style: **`bindFunc`** returns a small **`CallFuncInputBase`**; **`execFunc`** returns an **empty** **`execution::Context`** (or zero-row columns if the framework requires non-empty column metadata). No heavy work.

8. **Build wiring**  
   **Decision:** Add sources under **`src/compiler/function/gds/`**, extend **`gds/CMakeLists.txt`**, include new headers from **`function_collection.cpp`** and register **`TABLE_FUNCTION(...)`** entries.

## Risks / Trade-offs

- **[Risk] Binder change affects all `NeugCallFunction` with `TableFunction::bindFunc` set** → **Mitigation:** Default remains unchanged when **`bindFunc`** is null (current behavior for existing functions).
- **[Risk] Map literal internal representation (`Value`) may not match assumptions** → **Mitigation:** Unit tests or binder-level tests for one canonical literal form; document supported syntax in spec.
- **[Risk] `ANY` typing allows invalid argument shapes at bind time** → **Mitigation:** Explicit validation inside **`TableFunction::bindFunc`** with clear exceptions.

## Migration Plan

- No database migration. Session-only **`GraphEntrySet`**; behavior is additive for new **`CALL`** forms.

## Open Questions

- Exact **`common::Value`** layout for nested map literals in this codebase (field names, list vs map for rel triple keys like `[Person, KNOWS, Person]`).
- Whether **`CALL`** should expose zero columns or a single “ok” column for UX consistency with other side-effect procedures.
