## 1. 类型与 API

- [ ] 1.1 在 `include/neug/compiler/function/gds/` 引入 **`GDSAlgoFunction : public NeugCallFunction`**，声明 **`algo_exec_func_t`**（与 [`compiler-spec2.md`](../../../specs/004-gds/compiler-spec2.md) 一致或附 adapter 注释）。
- [ ] 1.2 将共享 **`bindGDSFunction(..., algo_name)`** 声明为 **`GDSAlgoFunction`** 使用的入口（实现可留在 `.cpp`）。

## 2. 构造与绑定

- [ ] 2.1 **`GDSAlgoFunction` 构造函数** 中设置 **`TableFunction::bindFunc`**（指向 **`bindGDSFunction`** 或包装），并保存 **`algo_exec_func_t`**。
- [ ] 2.2 设置 **`NeugCallFunction::execFunc`** 为桥接 lambda（首期可 stub，行为与当前 **`k_core`** 一致）。

## 3. 迁移 KCore

- [ ] 3.1 将 **`KCoreFunction::getFunctionSet()`** 从 **`std::make_unique<NeugCallFunction>`** 改为 **`std::make_unique<GDSAlgoFunction>`**。
- [ ] 3.2 删除重复的 **`static_cast<TableFunction*>` + 手动 `bindFunc`** 样板代码。

## 4. 验证

- [ ] 4.1 运行现有 compiler 测试（含 **`k_core` / extension** 相关用例），确认逻辑计划与物理计划不变。
- [ ] 4.2 （可选）为 **`GDSAlgoFunction`** 添加最小单元测试或注释说明 **`dynamic_cast<NeugCallFunction>`** 在 binder 中的必要性。
