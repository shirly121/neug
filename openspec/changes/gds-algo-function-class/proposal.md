## Why

[`specs/004-gds/compiler-spec2.md`](../../../specs/004-gds/compiler-spec2.md) sketches a dedicated **`GDSAlgoFunction`** type with **`algo_exec_func_t`** and a shared **`bindGDSFunction`** for compiler-time binding. Today, concrete GDS entry points (e.g. **`k_core`**) are implemented as plain **`NeugCallFunction`** with **`TableFunction::bindFunc`** wired manually. Introducing **`GDSAlgoFunction`** clarifies the contract for GDS algorithms, centralizes bind wiring, and leaves room for an execution signature that carries **`GDSAlgo` / `Subgraph`** context instead of only **`CallFuncInputBase`**.

## Feasibility (结论)

**可行。** 前提是让 **`GDSAlgoFunction` 仍然是一种 `NeugCallFunction`（公有继承）**，因为 binder 在 **`Binder::bindTableFunc`** 里把匹配的函数 **`constPtrCast<NeugCallFunction>`** 再读取 **`outputColumns`** 与 **`TableFunction::bindFunc`**。若 `KCore` 只继承 `TableFunction` 而不经过 `NeugCallFunction`，则需要改 binder / catalog 的多套分支，成本高且易出错。

**`algo_exec_func_t` 与现有 `call_exec_func_t` 不同**：同样可行，只要在 **`GDSAlgoFunction` 构造或注册时** 用一层 **adapter** 把 `NeugCallFunction::execFunc`（或引擎实际调用的路径）桥接到 `algo_exec_func_t`。引擎侧若暂时仍只认 `CallFuncInputBase`，可让 adapter 从 **`PhysicalPlan` + `op_idx`**（或扩展后的 input）里取出已下发的 **`GDSAlgo`**，再调用算法实现。

**`compiler-spec2.md` 里 63–73 行的示例代码**不能直接照抄：`NeuGCallFunction` 拼写、**`ptrCast<TableFunction>(func)`** 中的 **`func` 未定义**；正确做法是 **`static_cast<TableFunction*>(this)`** 或对 **`this->bindFunc`**（`TableFunction` 成员）赋值 **`bindGDSFunction`**（或按算法包装后的 bind）。

## What Changes

- 新增 **`function::GDSAlgoFunction`**（**`public NeugCallFunction`**），成员包含 **`algo_exec_func_t`**（或等价别名），构造函数中统一设置 **`TableFunction::bindFunc`** 为共享的 **`bindGDSFunction`**（或 `bindGDSFunction` 的 thin wrapper，携带算法名）。
- 将 **`k_core`**（及后续 GDS）的 **`getFunctionSet()`** 改为 **`std::make_unique<GDSAlgoFunction>(...)`**（或工厂），避免手写 **`NeugCallFunction` + static_cast**。
- （可选）扩展 **`CallFuncInputBase`** 子类以携带 **`GDSAlgo`** 或 **`GDSFuncBindData`** 快照，便于 **`algo_exec_func_t`** 与当前执行框架对齐。

## Capabilities

### New Capabilities

- **`compiler-gds-algo-function-type`**: 以 **`GDSAlgoFunction`** 作为 GDS **`CALL`** 的统一函数对象类型，与 **`bindGDSFunction` / `algo_exec_func_t`** 对齐设计文档。

### Modified Capabilities

- （无独立主 spec 目录时，以本 change 的 delta spec 为准。）

## Impact

- **Primary:** `include/neug/compiler/function/gds/`（**`GDSAlgoFunction` 声明**）、`src/compiler/function/gds/gds_algo_function.cpp`（**`k_core` 构造迁移**）。
- **Secondary:** 若引入新 **`CallFuncInputBase`** 派生类，**`execution`** 或 **`NeugCallFunction::bindFunc`** 路径需小幅配合。
- **Non-goals（本 change 可拆分）：** 完整实现 **`algo_exec_func_t`** 的引擎消费；可先保留 stub adapter。
