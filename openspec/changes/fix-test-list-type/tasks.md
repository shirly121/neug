## 1. Storage: ListRefColumn

- [ ] 1.1 Add **`ListRefColumn`** (or equivalent) in [`column.h`](include/neug/utils/property/column.h) implementing **`RefColumnBase`**, wrapping **`const ListColumn&`**, **`get(index)`** → **`get_prop(index)`**, **`type()`** → **`kList`**.
- [ ] 1.2 Extend **`CreateRefColumn`** in [`column.cc`](src/utils/property/column.cc) with **`case DataTypeId::kList`** using **`dynamic_cast<const ListColumn&>`**.

## 2. Schema / YAML (if needed)

- [ ] 2.1 If **`test_list_type`** or compaction still warns **Unsupported type in YAML** for list types, extend [`g_type_utils.h`](include/neug/compiler/gopt/g_type_utils.h) **`toYAML` / `createLogicalType`** for **`LogicalTypeID::LIST`** (minimal round-trip).

## 3. Tests

- [ ] 3.1 Remove any **`@pytest.mark.skip`** / commented skip on **`test_list_type`** in [`test_ddl.py`](tools/python_bind/tests/test_ddl.py); keep docstring describing DML + read-back.
- [ ] 3.2 Run **`pytest tools/python_bind/tests/test_ddl.py::test_list_type -v`** and fix any remaining failures (e.g. **`property.cc`** stream operator for list **`Property`** if hit on shutdown).
