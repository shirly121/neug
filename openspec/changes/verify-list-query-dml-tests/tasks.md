## 1. Document and align query tests

- [x] 1.1 In `tools/python_bind/tests/test_db_query.py` (or adjacent README comment block), document which tests assert **homogeneous** vs **heterogeneous** list `RETURN` (map to `test_to_tuple`, `test_nested_tuple`, `test_null_value_tuple`, and any new cases).
- [x] 1.2 Add **`test_return_homogeneous_string_list_literal`** (or equivalent) using **`tmp_path`** + minimal schema if `RETURN ['a','b']` without graph is supported; otherwise document skip reason in test docstring.
- [x] 1.3 Ensure **bulk-load** prerequisites for `/tmp/ldbc` and `/tmp/modern_graph` are referenced from **`openspec/changes/verify-list-query-dml-tests/design.md`** or a one-line pointer in `tests/conftest.py` / module docstring.

## 2. Document and align DML / list column tests

- [x] 2.1 Extend **`tests/test_ddl.py`** (or add `test_list_dml.py`): fix **`test_list_type`** to reassign `res = conn.execute(...)` before the second assertion when testing `e.values` (correctness bug).
- [x] 2.2 Add docstring or comment tying **`test_list_type`** to **DML list literal** + **read-back**; if `STRING[]` / ref column is unsupported, mark **`@pytest.mark.skip`** with issue link or engine TODO.
- [x] 2.3 Add a minimal **edge list property** scenario when rel `STRING[]` is supported, or document as future work in spec notes.

## 3. CI / developer ergonomics

- [x] 3.1 Cross-check **`.github/workflows/neug-test.yml`** order: DDL/query tests run after datasets where needed; no change required if already satisfied.
- [x] 3.2 Optional: add **`pytest` marker** `requires_bulk_data` for tests that need `/tmp/ldbc` and document in `pyproject.toml` `[tool.pytest.ini_options]` markers section.
