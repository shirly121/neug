# SQLite FTS implementation handoff

## Current milestone

- Completed: M0 — extension skeleton and test entry point.
- Completed: M1 — SQLite-free stub index and fused Cypher-to-index-scan path.
- M1 implementation reviewed and accepted: 2026-07-17.
- Completed: M2 — Open/Dump and minimal file persistence. The index
  follows the existing `Module` lifecycle and does not add a public `Close`
  operation.
- M2 implemented `Open`/`Dump` using the zvec/HNSW pattern: copy a persisted file
  into a unique runtime path during `Open`, flush and commit it through an
  `IDataContainer` adapter during `Dump`, and release the backend handle in
  the index destructor.
- M2's fixed test payload was removed in M3; the same runtime/checkpoint path
  now holds the SQLite database.
- Completed: M3 — the temporary file backend is replaced with a contentless
  SQLite FTS5 database opened directly at the runtime path. Inserts and MATCH
  queries use bound parameters, BM25 results retain their scores through
  version filtering, and both the database and index-ID accessor survive
  checkpoints.
- Next milestone: M4 — error-message consolidation, user documentation, and
  broader regression testing.

## M3 deliverables

- SQLite 3.53.3 is pinned as the `third_party/sqlite` submodule at commit
  `92a6c5c3636faa021ecc3be5403a00f50f65eda7`. Its official amalgamation is
  generated during the build and linked statically and privately to the
  extension and its native test target. Runtime probes verify both FTS5 and
  the exact SQLite version.
- Internal RAII wrappers own SQLite databases and prepared statements.
- Metadata validation covers the index name, STRING property, tokenizer,
  prefix lengths, detail mode, BM25 rank, and candidate batch size.
- Each index creates an isolated contentless `neug_fts_{name}` table whose
  rowid is the NeuG internal index ID.
- Ranked search binds the full MATCH expression, orders by BM25 ascending,
  over-fetches a bounded candidate batch, and filters superseded/deleted IDs
  without separating scores from their vertex IDs.
- Dump flushes and closes SQLite in DELETE-journal mode before checkpointing,
  then reopens a fresh writable runtime copy. Restores validate the target
  FTS5 table and can continue appending.
- Tests cover FTS5 availability, word/phrase/prefix/no-match queries, MATCH
  syntax errors, option validation, Upsert/Delete filtering, repeated Dump,
  direct restore, and full database close/reopen/append behavior.

## M0 deliverables

- `extension/sqlite_fts/CMakeLists.txt` defines:
  - `neug_sqlite_fts_extension`
  - `sqlite_fts_extension_test`
- `include/sqlite_fts_extension.h` owns the extension name constants.
- `src/sqlite_fts_extension.cc` exports `Init()` and `Name()` and registers the
  catalog extension name `sqlite_fts` with display name `SQLITE_FTS`.
- `tests/sqlite_fts_extension_test.cc` opens a temporary NeuG database and
  verifies that `LOAD sqlite_fts` succeeds.
- `extension/CMakeLists.txt` contains the approved root-build registration:
  `add_extension_if_enabled("sqlite_fts")`.

## Verification

Configuration:

```sh
cmake -S . -B build -DBUILD_TEST=ON -DBUILD_EXTENSIONS=sqlite_fts
```

Build:

```sh
cmake --build build --target sqlite_fts_extension_test -j8
```

M3 test:

```sh
ctest --test-dir build \
  -R 'sqlite_fts_extension_test|SQLiteFTS' \
  --output-on-failure
```

Result on 2026-07-17:

```text
100% tests passed, 0 tests failed out of 17
```

Both the extension library and test executable were produced under
`build/extension/sqlite_fts/`.

## Scope and constraints carried forward

- M0 contains no SQLite headers, libraries, or link items.
- Tests remain C++/gtest tests under `extension/sqlite_fts/tests/`, following
  the zvec extension pattern. Python tests are not used for this extension.
- The shared root build can be driven from `tools/python_bind/` when convenient,
  but extension tests remain native C++ targets.
- Do not modify `StorageIndex`, `StorageIndexManager`, or compiler core files
  without first documenting the exact framework gap and obtaining approval.
- M1 must still avoid introducing or linking SQLite.
- User-visible querying must follow the zvec-style shape documented in
  `specs/004-fts-index/tasks.md`; users do not pass an index name or invoke the
  internal scan function directly.
- `Clone` and `Detach` are not part of the current FTS milestone. They remain
  overridden because `Module` declares both as pure virtual operations, but
  both overrides explicitly return `NOT_SUPPORTED`. Do not add copy-on-write
  semantics until they are separately specified and reviewed.

## M1 deliverables

- `SQLiteFTSQueryParams` carries the intact query string and Top-K value.
- `SQLiteFTSIndex` is registered as `sqlite_fts_index` and returns deterministic
  `(vid, score)` stub results without linking SQLite.
- `SQLITE_FTS_BM25` is binder-visible but rejects standalone scalar execution.
- `SQLiteFTSIndexScanOptimizer` recognizes only the complete ascending Top-K
  query shape and replaces scan, scoring, ordering, and limit with one
  `SQLITE_FTS_INDEX_SCAN` operator.
- The fused operator resolves the index by label/property and returns node and
  score columns.
- Tests cover query parameter preservation, fused-plan shape, unsupported
  query forms, invalid arguments, missing indexes, and end-to-end results,
  following the zvec extension's direct-index and Cypher test style.

Verification:

```sh
cmake --build build --target sqlite_fts_extension_test -j8
ctest --test-dir build \
  -R 'sqlite_fts_extension_test|SQLiteFTS' \
  --output-on-failure
```

Keep the existing M0 load test in the suite as a regression test throughout
all later milestones.
