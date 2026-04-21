## Why

GDS / graph analytics needs **named projected subgraphs** that are not materialized in storage but registered in the compiler session as **aliases** (table names + string predicates). Without `CALL project_graph` and a matching drop, planners and later phases cannot refer to a stable graph name backed by `ClientContext::GraphEntrySet`. This change delivers the compiler-side registration and removal hooks described in [`specs/004-gds/compiler-spec.md`](../../../specs/004-gds/compiler-spec.md).

## What Changes

- Add **`CALL project_graph(graphName, nodeMap, relMap)`** as a **`NeugCallFunction`** that parses literal map arguments, validates input, and **inserts** a `graph::ParsedGraphEntry` into **`GraphEntrySet`** (no physical materialization).
- Add **`CALL drop_projected_graph(graphName)`** (or the catalog name aligned with implementation) as a **`NeugCallFunction`** that **removes** the named entry from **`GraphEntrySet`** if it exists, with clear errors when it does not.
- Register both functions in the built-in function catalog so **`CALL ...`** binding resolves them.
- Wire **compiler-side** validation (e.g. optional graph-entry binding / placeholder helpers per `compiler-spec.md`); **engine** `bindFunc` / `execFunc` remain minimal no-op or empty-result paths as required by the existing `NeugCallFunction` pattern.

## Capabilities

### New Capabilities

- `compiler-projected-graph`: Compiler registration of projected graph aliases via `project_graph` and removal via `drop_projected_graph`, scoped to `GraphEntrySet` and `NeugCallFunction` integration.

### Modified Capabilities

- (none — no existing `openspec/specs/` capabilities to delta.)

## Impact

- **Primary:** `src/compiler/function/gds/` (new `.h`/`.cpp`, `CMakeLists` wiring).
- **Secondary (within `src/compiler` only if required):** built-in function registration (e.g. catalog / function table entries), minimal helpers for extracting map literals into `GraphEntryTableInfo` / `ParsedGraphEntry`.
- **Out of scope per product note:** changes outside `src/compiler` unless explicitly justified; engine execution and storage materialization are not part of this change.
