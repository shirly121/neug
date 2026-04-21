## ADDED Requirements

### Requirement: CreateRefColumn supports list storage columns

The storage layer SHALL construct a valid **`RefColumnBase`** when the underlying column is a **`ListColumn`** (`DataTypeId::kList`), so list-typed properties can be read through the same reference interface as scalars and strings.

#### Scenario: Vertex list property read after insert

- **WHEN** a node table defines **`STRING[]`** (or equivalent list-of-string), a row is inserted with a list literal, and a query **returns** that property
- **THEN** **`CreateRefColumn`** SHALL not throw **unsupported reference column**, and the returned values SHALL match the inserted list elements

#### Scenario: Edge list property read after insert

- **WHEN** a relationship table defines a **list-typed** property, an edge is created with a list literal, and a query **returns** that property
- **THEN** the engine SHALL read back the list without **`CreateRefColumn`** failure, consistent with the vertex list behavior

### Requirement: Python regression test test_list_type passes

The **`test_list_type`** test in **`tools/python_bind/tests/test_ddl.py`** SHALL pass without **`pytest.mark.skip`** when run against a build that includes this change.

#### Scenario: Local pytest

- **WHEN** `python3 -m pytest tools/python_bind/tests/test_ddl.py::test_list_type` is executed after a clean build
- **THEN** the test SHALL **pass** (assertions on **`n.tags`** and **`e.values`**)
