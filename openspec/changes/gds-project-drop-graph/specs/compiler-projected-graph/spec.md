## ADDED Requirements

### Requirement: Register projected graph via CALL project_graph

The compiler SHALL accept an in-query **`CALL project_graph(graph_name, node_map, rel_map)`** where **`graph_name`** is a string literal and **`node_map`** / **`rel_map`** are literal values resolvable at bind time into node and relationship table descriptors (table identifier string + optional predicate string per entry). On successful bind, the system SHALL insert a **`ParsedGraphEntry`** into the session **`GraphEntrySet`** under **`graph_name`** without materializing storage.

#### Scenario: Successful registration

- **WHEN** the user issues **`CALL project_graph('g', {...}, {...})`** with valid literals and **`g`** is not already registered
- **THEN** bind completes and **`GraphEntrySet`** contains **`g`** with node and rel **`GraphEntryTableInfo`** consistent with the provided maps

#### Scenario: Duplicate graph name

- **WHEN** **`project_graph`** is called with a **`graph_name`** that already exists in **`GraphEntrySet`**
- **THEN** bind SHALL fail with a clear error and the existing entry SHALL remain unchanged

### Requirement: Remove projected graph via CALL drop_projected_graph

The compiler SHALL accept **`CALL drop_projected_graph(graph_name)`** with a string literal **`graph_name`**. On successful bind, the system SHALL remove that name from **`GraphEntrySet`** if present.

#### Scenario: Successful drop

- **WHEN** **`drop_projected_graph('g')`** is called and **`g`** exists
- **THEN** bind completes and **`GraphEntrySet`** no longer contains **`g`**

#### Scenario: Drop missing name

- **WHEN** **`drop_projected_graph('g')`** is called and **`g`** does not exist
- **THEN** bind SHALL fail with a clear error

### Requirement: Compiler scope and integration

Registration and removal SHALL be implemented in the compiler layer using **`NeugCallFunction`** and session **`ClientContext`** / **`GraphEntrySet`**, consistent with [`specs/004-gds/compiler-spec.md`](../../../../../specs/004-gds/compiler-spec.md). Execution-layer **`execFunc`** MAY be a no-op aside from satisfying the **`CALL`** protocol.

#### Scenario: Session alias only

- **WHEN** a projected graph is registered
- **THEN** no requirement is imposed that storage or engine materializes subgraph data as part of this capability
