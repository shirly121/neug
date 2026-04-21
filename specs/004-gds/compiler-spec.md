# Compiler 侧支持 project graph 功能

## Cypher API

```cypher
CALL project_graph(
    <GRAPH_NAME>,
    {
        <NODE_TABLE_0> :  <NODE_PREDICATE_0>,
        <NODE_TABLE_1> :  <NODE_PREDICATE_1>,
        ...
    },
    {
        <[SRC_NODE_TABLE, REL_TABLE, DST_NODE_TABLE]_0> :  <REL_PREDICATE_0>,
        <[SRC_NODE_TABLE, REL_TABLE, DST_NODE_TABLE]_1> :  <REL_PREDICATE_1>,
        ...
    }
);
```

示例：
```cypher
CALL project_graph(
    'filtered_graph',
    {'Person': 'n.name <> "Ira"'},
    {'[Person, KNOWS, Person]': 'r.id < 3'}
);
```

## 具体设计

### Project Graph

Compiler 侧不会真正物化子图，只是保存子图的别名，保存在 `GraphEntrySet` 接口中，代码中已有相关实现：

```
const graph::GraphEntrySet& ClientContext::getGraphEntrySet() const {
  return *graphEntrySet;
}
```

我们需要实现的是：通过 neug_call_function 接口实现 call project graph 功能，也就是将查询给到的 string/predicate 信息插入到 `GraphEntrySet` 中。

在 src/compiler/function/gds 目录下实现 project_graph_function:
```c++
struct ProjectGraphNativeFunction {
    static constexpr const char* name = "PROJECT_GRAPH";

    static function_set getFunctionSet();
};
```

重点就在于如何实现 neug_call_function，并且保存在 function_set 中，供框架调用。

```c++
function_set ProjectGraphNativeFunction::getFunctionSet() {
    function_set functionSet;
    auto func = std::make_unique<NeugCallFunction>(name,
        std::vector{LogicalTypeID::STRING, LogicalTypeID::ANY, LogicalTypeID::ANY});
    // 注意：这里有两个同名的 bindFunc，需要区分开
    // 第一个bindFunc是在compiler侧执行
    // 第二个bindFunc是在engine侧执行
    auto tableFunc = func->ptrCast<TableFunction>(func);
    // 设置compiler bind func，这个函数会将用户设置的string/predicate插入到GraphEntrySet中，需要重点实现
    tableFunc->bindFunc = compilerBindFunc;
    // 设置engine bind func，engine层几乎不做什么，返回一个默认值或者空值
    func->bindFunc = engineBindFunc
    // 设置engine执行的函数，do nothing
    func->execFunc = execFunc;
    functionSet.push_back(std::move(func));
    return functionSet;
}

static std::unique_ptr<TableFuncBindData> compilerBindFunc(const main::ClientContext*,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto nodeInfos = extractGraphEntryTableInfos(input->getValue(1));
    auto relInfos = extractGraphEntryTableInfos(input->getValue(2));
    auto bindData =  std::make_unique<ProjectGraphNativeBindData>(graphName, nodeInfos, relInfos);
    auto graphEntrySet = GraphEntrySet::Get(*input.context->clientContext);
    graphEntrySet->validateGraphNotExist(bindData->graphName);
    auto entry = std::make_unique<ParsedNativeGraphEntry>(bindData->nodeInfos, bindData->relInfos);
    // bind graph entry to check if input is valid or not. Ignore bind result.
    GDSFunction::bindGraphEntry(*input.context->clientContext, *entry);
    graphEntrySet->addGraph(bindData->graphName, std::move(entry));
}

// engineBindFunc和exeFunc几乎不做什么，只要保证接口返回的合理性即可
```

### Drop Graph

我们还需要实现一个drop graph功能，用于删除GraphEntrySet中的指定name的信息。同样我们需要设计一个 drop graph function:

```c++
struct DropProjectedGraphFunction {
    static constexpr const char* name = "DROP_PROJECTED_GRAPH";

    static function_set getFunctionSet();
};
```

重点在于怎么实现 compilerBindFunc:

```c++
function_set DropProjectedGraphFunction::getFunctionSet() {
    function_set functionSet;
    auto func = std::make_unique<NeugCallFunction>(name, std::vector{LogicalTypeID::STRING});
    // 注意：这里有两个同名的 bindFunc，需要区分开
    // 第一个bindFunc是在compiler侧执行
    // 第二个bindFunc是在engine侧执行
    auto tableFunc = func->ptrCast<TableFunction>(func);
    // 设置compiler bind func，这个函数会将用户设置的string/predicate插入到GraphEntrySet中，需要重点实现
    tableFunc->bindFunc = compilerBindFunc;
    // 设置engine bind func，engine层几乎不做什么，返回一个默认值或者空值
    func->bindFunc = engineBindFunc
    // 设置engine执行的函数，do nothing
    func->execFunc = execFunc;
    functionSet.push_back(std::move(func));
    return functionSet;
}

static std::unique_ptr<TableFuncBindData> bindFunc(const main::ClientContext*,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0 /* maxOffset */);
    auto bindData = std::make_unique<DropProjectedGraphBindData>(graphName);
    auto graphEntrySet = graph::GraphEntrySet::Get(*input.context->clientContext);
    graphEntrySet->validateGraphExist(bindData->graphName);
    graphEntrySet->dropGraph(bindData->graphName);
}
```

## 实现要点

- 请保证你的修改仅限于src/compiler下的相关内容，超出该范围的目录文件坚决不要修改。
- 大部分改动应该在src/compiler/function/gds下面，如果改动超出该范围，需要重点说明为什么？（比如使用了util/helper函数等）
- 对于不存在的util/helper函数，不用完整实现，按照需求创建相关文件和接口，先保留空实现，后面再实现。
- 设计过程中，可以进一步参考 004-gds/spec.md 文档，帮助你了解一些背景需求，全链路设计等。

## 测试

```cypher
CALL project_graph(
    'filtered_graph',
    {'Person': 'n.name <> "Ira"'},
    {['Person', 'KNOWS', 'Person']: 'r.id < 3'}
);
```