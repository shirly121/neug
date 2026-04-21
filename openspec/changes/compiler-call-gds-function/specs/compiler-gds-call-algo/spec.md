## ADDED Requirements

### Requirement: Lower GDS CALL to GDSAlgo physical operator

The compiler SHALL accept **`CALL <gds_algorithm>(graph_name, options_map) YIELD ...`** where **`graph_name`** is a string literal naming a session projected graph (**`GraphEntrySet`**) and **`options_map`** is a map literal of algorithm options (e.g. **`min_k`**, **`concurrency`**). On successful bind and planning, the physical plan SHALL contain an operator whose payload is **`GDSAlgo`** (protobuf), not a generic **`ProcedureCall`**, carrying **`algo_name`**, **`sub_graph`**, and **`options`** as described in [`specs/004-gds/compiler-spec2.md`](../../../../../specs/004-gds/compiler-spec2.md).

#### Scenario: k_core example

- **WHEN** the user issues **`CALL k_core('my_graph', {min_k: 3, concurrency: 4}) YIELD node, core_number`** and **`my_graph`** is registered in **`GraphEntrySet`**
- **THEN** binding succeeds and the physical plan includes **`GDSAlgo`** with **`algo_name`** **`k_core`**, **`options`** containing string entries for **`min_k`** and **`concurrency`**, and **`sub_graph`** consistent with the bound graph entry and schema

#### Scenario: Unknown graph name

- **WHEN** **`graph_name`** does not resolve to an entry in **`GraphEntrySet`**
- **THEN** bind SHALL fail with a clear error

### Requirement: GDSFuncBindData drives conversion

The compiler SHALL use a dedicated **`TableFuncBindData`** subclass (**`GDSFuncBindData`**) produced by **`bindGDSFunction`**, holding the bound **`GraphEntry`** (or equivalent) and normalized options. **`GQueryConvertor::convertTableFunc`** SHALL detect this bind data and invoke **`convertGDSFunction`** to populate **`GDSAlgo`**.

#### Scenario: Dispatch before procedure call

- **WHEN** a **`CALL`** resolves to a **`GDSAlgoFunction`** and bind data is **`GDSFuncBindData`**
- **THEN** the converter SHALL not emit **`ProcedureCall`** for that operator

### Requirement: Schema-bound subgraph

**`Subgraph`** in **`GDSAlgo`** SHALL use schema-resolved label ids and **`common::Expression`** predicates that are validated against the current **`Schema`**, as specified in [`specs/004-gds/compiler-spec2.md`](../../../../../specs/004-gds/compiler-spec2.md).

#### Scenario: Invalid property reference

- **WHEN** a predicate references a property that does not exist in the schema for the labeled pattern
- **THEN** bind SHALL fail with a clear error
