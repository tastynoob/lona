# Generic v0 Internals

本文记录当前仓库里已经 shipped 的 generic v0 内部模型。
它不再描述早期“前端已通、runtime 未通”的过渡状态，而是聚焦现在真实存在的行为边界：

- generic 语法节点怎样进入 AST / type node
- generic metadata 怎样进入接口与 artifact
- same-module / imported generic runtime instantiation 怎样生成 concrete IR
- single trait bound 怎样参与实例化与静态调用

用户侧语法和限制见：

- [../../reference/language/type.md](../../reference/language/type.md)
- [../../reference/language/trait.md](../../reference/language/trait.md)

generic 与 trait 打开后的符号表分层，见：

- [symbol_table.md](symbol_table.md)

## 1. 当前范围

当前 generic v0 已经覆盖：

- `struct Box[T]`
- `def id[T](value T) T`
- 类型位置的 `Box[i32]`
- 表达式侧的 `id[i32](...)`、`id(...)`、`@id[i32]`、`Box[i32](...)`
- same-module generic function / applied generic struct concrete instantiation
- imported generic function / applied generic struct 的跨模块 concrete instantiation
- structured generic instance key、同图 dedup、artifact 级 cache metadata 与 invalidation
- unconstrained `T` 的模板校验约束
- single trait bound `T Trait`
- declaration-site struct bound `struct Box[T Trait]`
- `impl[T Trait] Trait for Box[T] { ... }`
- generic struct method template 与实例化
- generic body 中的 trait-qualified static call，例如 `Hash.hash(&value)`
- bounded generic value 上的普通 bound method dot lookup，例如 `value.hash()`

当前仍然不做：

- multi-bound
- const generic
- generic trait
- default method / associated type

## 2. 语法与 AST / Type Node

generic v0 相关入口主要在：

- `grammar/main.yacc`
- `grammar/type.sub.yacc`
- `src/lona/ast/astnode.hh`
- `src/lona/ast/astnode_toJson.cc`
- `src/lona/ast/type_node_tools.cc`
- `src/lona/ast/type_node_string.cc`

当前关键表示包括：

- `AstStructDecl::typeParams`
- `AstFuncDecl::typeParams`
- `AstTraitImplDecl::typeParams`
- `AstTypeApply`
- `AppliedTypeNode`
- `AnyTypeNode`

当前表面语法分工已经收口为：

- 声明语法写 `[T]`
- `impl` header 的 self type 写 `Box[T]`
- 类型位置的 generic apply 也写 `Box[i32]`
- 表达式侧 generic apply 写 `id[i32](...)`、`@id[i32]`、`Box[i32](...)`

类型位置现在与数组共用 `[]`，内部按 bracket item 形状分流：

- 整数字面量维度走数组
- 类型节点走 applied generic type

当前还不支持“编译期常量维度”，所以 bracket item 不是整数维度、也不是合法类型时会直接报错。

## 3. 接口层与模板元数据

generic metadata 会进入 `ModuleInterface`，主要对应：

- `src/lona/module/module_interface.hh`
- `src/lona/module/module_interface.cc`
- `src/lona/declare/interface.cc`

接口层会记录：

- type / function / trait impl 的 type parameter 列表
- 每个 generic parameter 的可选 single bound
- generic struct method template metadata
- applied type 与 `any` 的 type-node 形状

generic struct method 不会在模板阶段直接变成 runtime method table；接口层保留的是模板级 method metadata，等具体 applied instance 请求出现时再 concrete 化。当前只有 struct method 支持 generic parameter；trait method 仍然不支持。

## 4. 类型解析与 concrete applied struct

类型解析相关路径主要在：

- `src/lona/module/compilation_unit.cc`
- `src/lona/type/type.hh`

当前规则是：

- bare template 不是 runtime type，`Box` / `Box*` 这类写法继续拒绝
- `Box[i32]` 会形成 concrete applied type request
- concrete applied struct 会 materialize 出真实字段布局、ABI 以及 concrete method symbol

这意味着下面这些路径现在都走 concrete runtime 语义，而不是早期的 opaque placeholder：

- local / field / parameter / return type 上的 `Box[i32]`
- `Box[i32](...)` 构造
- `box.get()` 这类实例化后的 inherent method 调用
- imported `dep.Box[i32]` 的 by-value declared type 与 method call

## 5. Generic Runtime Instantiation

generic runtime 的核心路径主要分布在：

- `src/lona/analyze/function.cc`
- `src/lona/module/compilation_unit.cc`
- `src/lona/emit/codegen.cc`

### 5.1 Function Instances

generic function 现在支持：

- explicit instantiation：`id[i32](1)`
- inferred instantiation：`id(1)`
- specialized function ref：`@id[i32]`
- imported 同构路径：`dep.id[i32](...)`、`dep.id(...)`、`@dep.id[i32]`

实例生成流程大致是：

1. 从 call / function-ref / declared type 形成 generic instance request
2. 用 structured key 查同图 registry
3. 命中则复用，未命中则按 substituted signature 重新 resolve / analyze
4. 生成 concrete HIR / LLVM function symbol

### 5.2 Applied Struct Instances

applied generic struct 现在支持：

- same-module by-value storage
- imported by-value storage
- concrete ctor lowering
- concrete inherent method lowering
- trait-qualified static call 命中 concrete applied receiver method

generic struct method 仍然不是“模板阶段直接发射”；它们是随着具体 applied instance 一起 concrete 化。

## 6. Emission Ownership, Dedup, Reuse, Invalidation

generic runtime 现在采用“structured key 去重 + 单一 emitter + requester 记录使用”的模型。

相关文件主要包括：

- `src/lona/module/generic_instance.hh`
- `src/lona/module/module_artifact.hh`
- `src/lona/workspace/workspace_builder.cc`

当前已经有三层相互对齐的模型：

- in-memory same-session instance key
- build graph 级 emission owner registry
- persisted artifact metadata

其中实例身份至少覆盖：

- template owner identity
- instance kind
- template identity
- concrete type arguments
- method name（若适用）

而 artifact record 还会额外记录：

- requester module identity
- template revision
- owner 可见 import 状态
- requester 可见 trait-impl 状态

这里有一个关键约束：

- `requester` 参与 artifact 记录和缓存复用判断
- 但不参与“这是不是同一个 concrete instance”的 identity

这让 local / imported 的实例请求都能归一到同一种 key，而不是依赖 mangled symbol display string 或 `Box[i32]` 这类纯显示名。

artifact reuse 现在会验证 generic instance record 的 revision 是否仍然匹配当前模板状态，因此：

- owner body change
- owner interface change
- owner-visible import state change
- requester-visible trait impl state change

都会让旧 concrete instance 记录失效。

## 7. Template Validation 与 Unconstrained `T`

模板校验与 generic capability 传播主要落在：

- `src/lona/resolve/resolve.cc`
- `src/lona/analyze/function.cc`

当前 unconstrained `T` 仍然严格受限：

- 允许：`T`、`T*`、`T const*`、`Box[T]`、`sizeof[T]()`
- 拒绝：`obj.method()`、`obj.field`、`obj + other`、`obj == other`

这个限制不只覆盖直接表达式，也覆盖别名传播后的结果，例如：

- generic helper return alias
- applied generic helper return alias
- generic struct method return alias

模板阶段的目标仍然是“先守住静态能力边界”，而不是把失败路径降格成 runtime fallback。

## 8. Single Trait Bound

single bound 相关逻辑现在已经接通：

- `T Trait`
- `struct Box[T Trait]`
- `impl[T Trait] Trait for Box[T] { ... }`
- `def method[U Trait](...)` 这类 generic struct method
- instantiation-site bound satisfaction
- generic body 中的 direct bound method call
- trait-qualified static call in generic body

当前语义模型是：

1. 声明处记录每个 type parameter 的可选 single bound
2. 实例化时检查 concrete type 是否有 visible impl
3. 通过后，generic body 允许 `value.method()` 这种“点选后立即调用”的
   bound method call，也允许显式写 `Trait.method(&value, ...)`

当前故意不做：

- multi-bound
- generic trait method

## 9. 当前内部边界

generic v0 现在已经是 shipped runtime feature，而不是 placeholder 前端。

但内部边界仍然明确：

- 不做 const generic
- trait method 不做 generic parameter
- 不做 runtime dictionary 主路径
- 不把 static generic failure 自动回退成 `Trait dyn`

如果后续继续扩展，最自然的方向是：

- multi-bound
- 更强的随机 / 增量回归覆盖
- 针对 runtime state lifetime 的额外验证
