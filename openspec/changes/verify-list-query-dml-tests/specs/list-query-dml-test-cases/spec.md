## ADDED Requirements

### Requirement: Query tests cover homogeneous list literals

The test suite SHALL include at least one scenario where a `RETURN` list literal has **uniform element types** and the result is validated as a **list** of that element type (behavioral check via Python `pytest`).

#### Scenario: String list literal without MATCH

- **WHEN** the engine supports `RETURN` of a literal list with identical string elements (e.g. `RETURN ['1', '2']` or equivalent)
- **THEN** a pytest SHALL assert the returned row matches the expected list value (e.g. `['1', '2']`)

#### Scenario: Same-type properties from a bound node

- **WHEN** a query returns `[n.p1, n.p2]` where both properties have the **same** logical type on a populated graph
- **THEN** a pytest SHALL assert the result is a **list** (not a struct/tuple-shaped encoding) consistent with client conventions

---

### Requirement: Query tests cover heterogeneous list literals (tuple semantics)

The test suite SHALL include at least one scenario where a `RETURN` list mixes **distinct** logical element types and the result is validated as a **tuple/struct**-shaped value per client encoding.

#### Scenario: Mixed-type properties from LDBC or modern_graph

- **WHEN** a graph is loaded (e.g. `/tmp/ldbc` or `/tmp/modern_graph` after bulk load per CI) and a query returns `[n.firstName, n.gender, n.birthday]` or `[n.name, n.age]`
- **THEN** a pytest SHALL assert a non-empty result and structure consistent with **heterogeneous** list semantics (existing tests such as `test_to_tuple`, `test_nested_tuple`, `test_null_value_tuple` satisfy or partially satisfy this)

---

### Requirement: DML tests cover list literals on write paths

The test suite SHALL document and, where the engine supports list-typed columns, verify that **property values** written as list literals round-trip on read.

#### Scenario: CREATE with list-typed property

- **WHEN** a DDL defines a list property (e.g. `STRING[]`) and a `CREATE` supplies a homogeneous list literal
- **THEN** a pytest SHALL create a fresh database (e.g. `tmp_path`), insert the row, `MATCH ... RETURN` the property, and assert equality to the written list **or** skip with reason if list columns are not yet supported end-to-end

#### Scenario: Edge property list literal

- **WHEN** a rel table defines a list-typed property and a `CREATE` pattern sets it via a list literal
- **THEN** a pytest SHALL assert read-back matches written values **or** skip with reason until supported

---

### Requirement: Test documentation references CI data prerequisites

Project documentation for these tests SHALL state that tests targeting **`/tmp/ldbc`**, **`/tmp/modern_graph`**, etc. require **bulk load** as in [`.github/workflows/neug-test.yml`](.github/workflows/neug-test.yml) (Phase 2), and that self-contained tests use **`tmp_path`** to avoid hidden dependencies.

#### Scenario: Developer runs pytest locally

- **WHEN** a developer runs `tests/test_db_query.py` without preloaded graphs
- **THEN** documentation (OpenSpec design/proposal and the repository **run-test** Agent skill) SHALL explain the required **`bulk_loader`** commands or **`gstest`** clone for LDBC

---

## Implementation notes (future work)

### Edge rel property `STRING[]` (full round-trip)

When execution fully supports list-typed **edge** properties end-to-end (storage + `RETURN e.values`), extend **`tests/test_ddl.py`** (or split file) with an assertion mirroring node `tags STRING[]` behavior. Until then, node/edge list columns may remain **skipped** in pytest with an engine TODO in the skip reason.
