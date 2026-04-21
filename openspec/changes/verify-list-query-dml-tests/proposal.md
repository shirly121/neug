## Why

Homogeneous list literals (`ToList`) and heterogeneous list literals (`ToTuple`) must stay correct across **read paths** (e.g. `RETURN [...]`) and **write paths** (e.g. `CREATE` / property values). Without an explicit **documented test matrix**, regressions are easy to miss—especially when CI tests depend on **bulk-loaded** graphs under `/tmp/ldbc` and `/tmp/modern_graph` per [`.github/workflows/neug-test.yml`](.github/workflows/neug-test.yml).

## What Changes

- Add a **specification of test scenarios** (query + DML) that validate list/tuple semantics and round-trip values.
- Add **implementation tasks** to add or align **pytest** cases in `tools/python_bind/tests/` (and note any **C++** golden tests if introduced later).
- Cross-link **data prerequisites** (bulk load before tests that open `/tmp/ldbc`, etc.) and the [`run-test`](.cursor/skills/run-test/SKILL.md) skill flow where relevant.

## Capabilities

### New Capabilities

- `list-query-dml-test-cases`: Documented and trackable tests for list literals in RETURN and in DML property assignments, covering homogeneous vs heterogeneous element types where the binder distinguishes them.

### Modified Capabilities

- (none)

## Impact

- `tools/python_bind/tests/` (new or extended tests)
- Optional: `openspec/changes/physical-plan-to-list-vs-to-tuple/` remains the feature change; this change **only** adds test documentation and task checklist for validation (can be implemented together or after).
