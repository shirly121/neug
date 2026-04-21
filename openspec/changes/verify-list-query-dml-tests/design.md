## Context

- **Query**: `ListCreationFunction` binds `LIST` when element types match, `STRUCT` when they differ; IR uses `ToList` vs `ToTuple` after the physical-plan change.
- **DML**: `InsertVertex` / property mappings use the same `GExprConverter` path for list literals on properties (e.g. `tags: ['a','b']`).
- **CI**: Python tests that open fixed paths (`/tmp/ldbc`, `/tmp/modern_graph`) require **bulk load** first (see [`.github/workflows/neug-test.yml`](../../../.github/workflows/neug-test.yml) **Phase 2** before **Phase 3** `tests/test_db_query.py`). Self-contained tests should use **`tmp_path`** and `CREATE NODE TABLE` where possible.

## Goals / Non-Goals

**Goals:**

- Define a **clear matrix** of scenarios: homogeneous list, heterogeneous tuple, nested lists, DML list property.
- Map each scenario to **pytest location** (file + test name pattern) or “to be added”.
- Note **environment** (preloaded DB vs `tmp_path`).

**Non-Goals:**

- Changing binder or IR semantics (covered by `physical-plan-to-list-vs-to-tuple`).
- Replacing existing E2E bulk-loader documentation wholesale.

## Decisions

- **Prefer `tmp_path` + DDL** for new DML list tests so they run without `gstest`; keep **existing** `test_db_query` / LDBC tests as regression on bulk-loaded data.
- **Homogeneous**: e.g. `RETURN ['1','2']` or `RETURN [n.a, n.b]` with same-type properties; expect **list** value shape in Python client.
- **Heterogeneous**: e.g. `RETURN [n.name, n.age]` or mixed-type literals; expect **tuple/struct** shape consistent with current client encoding.
- **DML**: e.g. `CREATE NODE TABLE ... tags STRING[]` + `CREATE (... { tags: ['x','y'] })` + `MATCH ... RETURN n.tags` (subject to engine support for list columns).

## Risks / Trade-offs

- **List column storage** (`STRING[]`) may still be partially unsupported in some layers—tests should `skip` or live behind capability checks until green.
- Duplicating large LDBC queries increases maintenance cost; prefer **minimal** literals on tiny schemas.

## Open Questions

- Whether to assert **physical plan** `to_list` / `to_tuple` via debug API; default is **behavioral** assertions on query results only.

## CI order (cross-check)

**Verified:** [`.github/workflows/neug-test.yml`](../../../.github/workflows/neug-test.yml) runs **Bulk Loader — Prepare test data** (Phase 2) before **Run Python Test** steps that invoke `tests/test_db_query.py`, so `/tmp/ldbc` and `/tmp/modern_graph` exist when `@pytest.mark.requires_bulk_data` tests run in CI.
