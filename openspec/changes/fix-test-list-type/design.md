## Context

- **`CreateColumn`** already supports **`kList`** via [`ListColumn`](include/neug/utils/property/column.h) (`set_any` / `get_prop` use `Property::from_list_data` / list blobs).
- **`CreateRefColumn`** uses **`TypedRefColumn<T>`** for fixed POD types and a special case for **`StringColumn`**. **`ListColumn`** is not dispatched; `default` throws **unsupported reference column**.
- **`test_list_type`** flow: `CREATE NODE TABLE` / `CREATE REL TABLE` with `STRING[]`, `CREATE` with list literals, `RETURN n.tags` / `RETURN e.values`.

## Goals / Non-Goals

**Goals:**

- Implement **`ListRefColumn`** (name illustrative) implementing **`RefColumnBase`**: `get(i)` delegates to **`ListColumn::get_prop(i)`** or equivalent, `type()` returns **`kList`**.
- **`CreateRefColumn(const ColumnBase&)`**: `dynamic_cast` to **`ListColumn`** and return the new ref wrapper; validate failure mode if wrong dynamic type.
- Green **`pytest`** for **`test_list_type`**.

**Non-Goals:**

- Redesigning on-disk list encoding (reuse **`ListColumn`** layout).
- Full **ARRAY** vs **LIST** SQL semantics beyond current binder/storage.

## Decisions

- **Wrapper pattern**: Mirror **`TypedRefColumn`** — hold **`const ListColumn&`** (or pointers to **`items_`/`data_`** via public **`get_view`/`get_prop`** only) to avoid duplicating mmap logic. Prefer **`get_prop`** for a single implementation of list → **`Property`**.
- **Casting**: In **`CreateRefColumn`**, add **`case DataTypeId::kList`** with **`dynamic_cast<const ListColumn&>`**; on failure, throw with a clear message.
- **YAML warning**: If schema dump still logs **Unsupported type in YAML: 4** for **`LogicalTypeID::LIST`**, add explicit **`toYAML` / `createLogicalType`** branch so graph metadata round-trips; treat as **follow-up** if tests pass without it.

## Risks / Trade-offs

- **`RefColumnBase::type()`** returns **`DataTypeId`** only; callers that need **element type** may still use **`Property`** payload — verify **execution** paths only need **`kList`** + value.
- **Edge** vs **vertex** tables both use the same column stack; one fix should cover both **unless** edge path bypasses **`CreateRefColumn`**.

## Open Questions

- Whether **any** code path assumes **RefColumn** is only scalar — audit **grep CreateRefColumn** after implementation.
