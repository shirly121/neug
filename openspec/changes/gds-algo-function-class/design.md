## Context

- **`NeugCallFunction`**（[`neug_call_function.h`](../../../include/neug/compiler/function/neug_call_function.h)）继承 **`TableFunction`**，并额外提供 **`call_bind_func_t` / `call_exec_func_t`** 与 **`outputColumns`**。
- **`Binder::bindTableFunc`**（[`bind_table_function.cpp`](../../../src/compiler/binder/bind/bind_table_function.cpp)）在匹配函数后 **`constPtrCast<NeugCallFunction>`**，并依赖 **`TableFunction::bindFunc`** 生成自定义 **`TableFuncBindData`**（如 **`GDSFuncBindData`**）。
- **`compiler-spec2.md`** 中的 **`algo_exec_func_t`** 草签名为：  
  `execution::Context(execution::Context& ctx, const pb::Subgraph&, const options_t&, const StorageReadInterface& graph)`  
  与现有 **`call_exec_func_t`**：  
  `execution::Context(const CallFuncInputBase& input, IStorageInterface& graph)`  
  不同。

## Goals

- 提供 **`GDSAlgoFunction`**，使每个 GDS 算法注册处 **显式** 使用该类型，而不是原始 **`NeugCallFunction`**。
- 集中设置 **`TableFunction::bindFunc -> bindGDSFunction`**（或带 **`algo_name`** 的闭包），减少重复与错误。
- 为 **`algo_exec_func_t`** 预留位置；短期可通过 **adapter** 接到现有 **`execFunc`**。

## Design

### 1. 类层次

```
TableFunction
  └── NeugCallFunction
        └── GDSAlgoFunction   // 仅 GDS CALL 使用
```

**不得**让 **`GDSAlgoFunction`** 脱离 **`NeugCallFunction`** 除非同步修改 binder。

### 2. 构造函数职责

- 调用基类 **`NeugCallFunction(name, inputTypes, outputColumns)`**。
- 设置 **`TableFunction::bindFunc`**：  
  `bindFunc = [algo = name](ClientContext* ctx, const TableFuncBindInput* in) { return bindGDSFunction(ctx, in, algo); };`  
  或 **`bindGDSFunction`** 从 **`input`** / catalog 解析算法名（若 CALL 名与算法名总一致，可只用函数 **`name`**）。
- 成员 **`algo_exec_func_t algoExec`**：由具体算法在构造时传入；**`NeugCallFunction::execFunc`** 赋值为 lambda，内部调用 **`algoExec`**（见 §3）。

### 3. `algo_exec_func_t` 与 `execFunc` 桥接

**选项 A（推荐，侵入小）：**  
暂不改变运行时调用签名，**`GDSAlgoFunction`** 仍将 **`NeugCallFunction::execFunc`** 设为包装函数；包装函数内构造 **`GDSAlgo`** 需要的上下文（从 **`PhysicalPlan`**、`op_idx`、`CallFuncInputBase` 派生类读取），再调用 **`algoExec`**。

**选项 B（与 spec 完全一致）：**  
扩展 **`CallFuncInputBase`**（例如 **`GDSCallFuncInput`**），在 **`NeugCallFunction::bindFunc`**（`call_bind_func_t`）里 **`make_unique<GDSCallFuncInput>(...)`** 填入 **`Subgraph` / options**；**`execFunc`** 里 **`dynamic_cast`** 后交给 **`algoExec`**。需要执行层 **`plan_parser` / procedure** 路径能填充该 input。

首期可采用 **选项 A 的 stub**（空 **`Context`**），与当前 **`k_core`** 行为一致，后续再演进到 **选项 B**。

### 4. `bindGDSFunction`

- 建议签名：  
  `std::unique_ptr<GDSFuncBindData> bindGDSFunction(ClientContext*, const TableFuncBindInput*, std::string_view algoName);`  
  内部完成：**`GraphEntrySet` 校验**、**`GDSFunction::bindGraphEntry`**、**options map 解析**、**binder 生成 YIELD 列**（与现实现一致）。
- **`KCoreFunction::getFunctionSet()`** 返回 **`std::make_unique<GDSAlgoFunction>(KCoreFunction::name, types, outputs, kCoreAlgoExec)`**。

### 5. 与 spec 示例的差异说明

- 文档中的 **`ptrCast<TableFunction>(func)`** 应理解为：在 **`GDSAlgoFunction`** 体内对 **`this`** 设置 **`TableFunction::bindFunc`**，无需未定义的 **`func`** 变量。
- **`NeuGCallFunction`** 应为 **`NeugCallFunction`**（项目实际类型名）。

## Risks

- **`algo_exec_func_t`** 若长期与 **`call_exec_func_t`** 并存，需文档化哪条路径为 “source of truth”，避免双实现。
- **`std::function` 捕获** 算法名会增加 **`TableFunction` 可复制语义下的体积**；若 **`TableFunction` 要求可拷贝，需确认 **`bindFunc` 拷贝行为** 符合预期。

## Open Questions

- 执行阶段是否统一从 **`PhysicalPlan`** 的 **`gds_algo`** 算子反序列化 **`Subgraph`**，再交给 **`algo_exec_func_t`**？
- **`StorageReadInterface`** 与现有 **`IStorageInterface`** 是否同一抽象；若否，adapter 层做映射。
