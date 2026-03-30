# 模块成员解析边界

本文档记录 `module.xxx` 这条路径当前在编译器里的真实落点、它仍然存在的分层问题，以及后续应当推进到的目标形态。

它是 [name_lookup_consistency.md](name_lookup_consistency.md) 的一个更具体补充，聚焦于：

- imported 模块成员访问
- `resolve` 与 `analysis / HIR lowering` 的职责边界
- “哪些语义应在 HIR 之前被抹除”

## 1. 问题背景

当前编译器已经把“本地顶层名字”和“imported 模块成员名字”的查找入口收敛到了同一组模块级查询接口上：

- `CompilationUnit::lookupTopLevelName(...)`
- `ModuleInterface::lookupTopLevelName(...)`

这一步已经解决了大量“本地路径”和“模块限定路径”走两套搜索顺序的问题。

这条路径现在已经进一步收口：

- `module.xxx` 会在 `resolve` 阶段直接折叠成具体语言实体
- `analyze/function.cc` 只消费已解析的 selector 结果，不再负责 imported 模块成员查询

当前剩下的相关边界主要是：

- bare `module` 仍然只停留在 `resolve` 绑定层，用于 `module(...)`、`var x = module` 这类 targeted diagnostic
- 值字段 / 方法 / injected member 仍然由 `analysis` 负责

## 2. 当前真实行为

当前实现里，`module.xxx` 不会进入普通 selector lookup 主路径。

这条链路现在是：

1. `resolve` 把裸模块别名解析成 `ResolvedEntityRef::module`
2. `resolve` 在处理 `AstSelector(module, member)` 时，若 parent 是 imported 模块别名：
   - 命中函数则直接绑定成 `ResolvedEntityRef::globalValue(...)`
   - 命中类型则直接绑定成 `ResolvedEntityRef::type(...)`
   - 未命中则直接报 `unknown module member`
3. `analysis` 优先读取 selector 的 resolved binding，并直接物化成：
   - `HIRValue(Function)`
   - `HIRValue(TypeObject)`

只有下面两类情况才会真正生成 `HIRSelector`：

- 值字段访问
- 方法选择器

## 3. 为什么这仍然是个问题

这条主路径已经按预期前移到 `resolve`，但仍有两个需要长期记住的工程约束：

### 3.1 bare module 仍然不是运行时值

当前实现已经不再把 bare `module` 物化成 `HIRValue(ModuleObject)`。

这条路径现在是：

- `resolve` 仍然会把 bare 模块别名绑定成 `ResolvedEntityRef::module`
- `analysis` 在 `analyzeField` / `analyzeCall` 入口直接对 bare `module` 给 targeted diagnostic
- `module` 不再进入 HIR，也不再进入 `sema::EntityRef`

因此，后续仍应保持：

- `module.xxx` 在 HIR 前消解
- bare `module` 只作为前端诊断语义，不参与 HIR 值语义

### 3.2 selector 主路径不应再回退

当前 `analysis` 的 selector 主路径应只负责：

- 值字段访问
- 方法选择器
- injected member
- 未来真正依赖类型信息的成员能力

如果 imported 模块 selector 再次流入这条路径，应视为编译器内部错误，而不是正常分派分支。

## 4. 目标收口

这轮收口后的目标边界已经是：

- imported 模块成员访问在 `resolve` 阶段就收敛成具体实体
- `analysis` 不再负责为 `module.xxx` 做模块接口查找
- HIR 中不再需要把模块命名空间作为表达式级占位继续往后传

更具体地说：

- `module.func` 在进入 HIR 前，就应该已经等价于“某个确定的全局函数实体”
- `module.Type` 在进入 HIR 前，就应该已经等价于“某个确定的类型实体”
- `analysis` 里保留 selector 分派的部分，应只剩下：
  - 值字段
  - 方法选择器
  - injected member
  - 未来真正依赖类型信息的成员能力

## 5. 非目标

这轮收口不应试图一次解决所有 selector 语义。

以下内容仍然可以继续留在 `analysis`：

- 结构体字段访问
- 方法选择器
- tuple 字段
- injected member
- `Type.xxx` 这类未来静态类型成员能力

原因是这些路径要么依赖值类型，要么依赖尚未实现的静态成员模型。

因此，这轮收口的范围应明确限定为：

- 只前移 `module.xxx`
- 不把所有 `AstSelector` 一次性塞进 `resolve`

## 6. 建议实现方向

### 6.1 在 `resolve` 中为 `AstSelector` 增加绑定结果

当前 `ResolvedFunction` 已经为下面几类语法节点保存了解析结果：

- `AstField`
- `AstFuncRef`

下一步应增加对 `AstSelector` 的绑定表，例如：

- `bindSelector(const AstSelector *, ResolvedEntityRef)`
- `selector(const AstSelector *) -> const ResolvedEntityRef *`

### 6.2 只在“parent 是 imported 模块”时折叠 selector

`resolve` 在处理 `AstSelector(parent, field)` 时：

1. 先解析 `parent`
2. 如果 `parent` 已经被解析成 imported 模块别名
3. 再用统一模块查询接口查 `field`
4. 若命中函数或类型，则直接把整个 selector 绑定成具体实体

当前实现已经按这个方式落地，可以把：

- `math.inc`
- `math.Counter`

在 `resolve` 阶段直接折叠成：

- `ResolvedEntityRef::globalValue("math.inc")`
- `ResolvedEntityRef::type("math.Counter")`

### 6.3 `analysis` 优先消费已解析 selector

`analysis` 中的两条主路径都应先检查 selector 是否已有 resolve 结果：

- `analyzeSelector(AstSelector *)`
- `analyzeCall(AstFieldCall *)` 中 `callee` 为 `AstSelector` 的分支

若 selector 已经在 `resolve` 中被折叠：

- 直接物化成具体 `HIRValue(Function)` 或 `HIRValue(TypeObject)`
- 不再重新走 `lookupMember(...)`

### 6.4 模块 lookup 从 selector 主路径中剥离

当前实现里，`lookupMember(...)` 的职责已经收窄为：

- 值成员
- 方法
- injected member

也就是说，模块成员查询已经不再属于 HIR lowering 的核心 selector 分派逻辑。

## 7. 需要保持的语义约束

即使把 `module.xxx` 前移到 `resolve`，下面这些规则也不能变化：

- imported 模块成员仍然只能访问模块导出的顶层函数和类型
- `module(...)` 仍然应该给出 targeted diagnostic，而不是把模块命名空间当成可调用值
- `module.Type(...)` 仍然应当沿用当前“类型名独占构造入口”的语义
- bare `module` 不应再物化成任何 HIR value / `EntityRef`
- imported 模块别名遮蔽规则仍然要保持稳定

换句话说，这次调整只改变职责边界，不改变语言表面规则。

## 8. 验收标准

当前实现应满足下面这些条件：

1. `resolve` 能为 `module.xxx` 直接产出具体实体绑定。
2. `analysis` 不再通过模块命名空间去查询 `module.xxx`。
3. `lookupMember(...)` 不再承担 imported 模块成员访问的主解析职责。
4. HIR 主路径不再依赖 `HIRValue(ModuleObject)` 来解释模块成员 selector 或 bare module。
5. 现有 `module(...)`、`module.func(...)`、`module.Type(...)` 的诊断与调用行为不回退。

## 9. 当前实现状态备注

当前仍然需要长期记住下面这点，避免误判现状：

- 现在的问题不是“模块成员访问被 lower 成了 `HIRSelector`”
- 当前主问题已经改成“bare module 只允许停留在前端诊断层，不允许形成 HIR / `EntityRef` 值”

这两者看起来相近，但工程含义不同：

- 前者是 HIR 形状错误
- 后者是语义占位边界问题

当前仓库已经解决了前面的“分层过晚”问题，剩下的是继续保持边界不回退。
