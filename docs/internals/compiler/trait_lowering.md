# Trait v0 Lowering

本文记录当前仓库里已经实现的 trait v0 内部模型，目标是回答两类问题：

- `Trait.method(&value, ...)` / `Trait.method(ptr, ...)` 为什么是零额外运行时成本
- `Trait dyn` / `cast[Trait dyn](&value)` 在前端和 LLVM IR 中到底怎样落地

这不是设计草案；这里描述的是当前代码已经在做的事。
这里也不会重复讲用户侧语法规则；语法和语言层语义请看
`docs/reference/language/trait.md`。

generic v0 当前已经有单独的内部文档：

- `docs/internals/compiler/generic_v0.md`

因此本文只讨论 trait v0 自己的 lowering；generic template、applied type
和当前未实现的 monomorphization 不在这里展开。

## 1. 范围

trait v0 当前只覆盖：

- `trait` 顶层声明
- `impl Trait for Type { ... }`
- `impl[T Trait] Trait for Box[T] { ... }`
- `Trait.method(&value, ...)` / `Trait.method(ptr, ...)` 静态限定调用
- `value.Trait.method(...)` / `ptr.Trait.method(...)` 显式 receiver trait 路径
- trait 方法参与普通 `obj.method()` 的唯一匹配
- `Trait dyn` 非 owning trait object
- `cast[Trait dyn](&value)` 借用式构造

明确不包含：

- multi-bound generic constraints
- generic trait
- default method
- associated type
- owning trait object

## 2. 接口层数据模型

trait 信息首先进入 `ModuleInterface`，对应文件：

- `src/lona/module/module_interface.hh`
- `src/lona/module/module_interface.cc`
- `src/lona/declare/interface.cc`

当前接口层新增了三类数据：

- `ModuleInterface::TraitDecl`
- `ModuleInterface::TraitMethodDecl`
- `ModuleInterface::TraitImplDecl`

同时，`ModuleInterface::TopLevelLookupKind` 现在多了一档 `Trait`，因此 importer 在查 `dep.Hash` 时，不会再退回到“函数 / 全局 / 类型”三选一的旧分支。

`declare/interface.cc` 在接口收集阶段做两件事：

1. 收集并验证 trait declaration：
   - trait body 只能稳定产出方法签名；
   - 字段、`var`、`global`、可执行语句和 nested struct 都会被定向拒绝；
   - trait method 不能带 body。
2. 收集并验证 impl declaration：
   - `impl Trait for Type { ... }` 会检查 orphan rule；
   - 同一可见程序图中的 `(Trait, Type)` 不能重复；
   - 编译器会验证 impl body 是否完整覆盖 trait 已声明的方法；
   - 每个 body method 都会按 name、receiver access、参数个数、binding kind、参数类型和返回类型与 trait 方法签名对齐。

## 3. `interfaceHash` 与可见接口边界

trait v0 没有把这些信息放进“实现细节”层，而是明确放进模块接口：

- trait declaration 本身进入 `interfaceHash`
- trait method 签名进入 `interfaceHash`
- visible `impl Trait for Type { ... }` declaration 进入 `interfaceHash`

这样做的原因是：

- importer 的 `Trait.method(&value, ...)` 解析依赖 trait 方法签名
- importer 的 `cast[Trait dyn](&value)` 和 `h.method()` 依赖 visible impl declaration
- 如果这些变化只落到 `implementationHash`，就会把 stale importer 或 stale artifact 复用成错误结果

当前仓库已经有 trait 专属的增量回归，覆盖：

- trait 方法签名变化会让 importer 重新编译
- visible impl declaration 增删会让 importer 重新编译
- 同一 `CompilerSession` 中不会把旧 trait/impl 接口误复用给 caller

## 4. 静态路径：`Trait.method(&value, ...)`

静态限定调用的链路是：

1. `resolve`
   - bare trait 名会解析成独立的 trait namespace binding
   - imported 路径里的 `dep.Hash` 也会在这里直接绑定成 trait
2. `analyze/function.cc`
   - 只在 `Trait.method(&value, ...)` / `Trait.method(ptr, ...)` 这种限定调用语境里消费 trait binding
   - 先找到 visible trait declaration
   - 再验证显式 self pointer 指向的 concrete type 是否有 visible impl
   - 最后把调用直接绑定到 concrete trait impl method
3. `emit/codegen.cc`
   - 直接生成对具体方法符号的普通调用

因此这条路径没有：

- witness table 读取
- 间接跳转
- 对象布局膨胀

普通 `Point` 仍然只是普通 `Point`；trait 不会往 struct 里注入隐藏 vptr。

## 5. `Trait dyn` 的语义类型

`Trait dyn` 在类型层不是语法糖，而是单独的内部类型 `DynTraitType`，对应：

- `src/lona/type/type.hh`
- `src/lona/type/type.cc`
- `src/lona/module/compilation_unit.cc`

`DynTraitType` 保存的是“目标 trait 的已解析名字”，而不是某个具体 self type。这样 importer 和本模块都可以把：

- `Hash dyn`
- `dep.Hash dyn`

收敛到同一个“按 trait 名命名”的语义类型。

当前实现不再按“整个 trait 是否 dyn-compatible”一刀切，而是把 receiver mutability 放到方法级别：

- getter-style trait method 对应只读擦除 receiver
- `set def` trait method 对应可写擦除 receiver
- 只读 `Trait const dyn` 只能调用 getter-style 方法
- 可写 `Trait dyn` 可以调用 getter 和 setter

## 6. trait object 的 HIR 形状

动态路径在 HIR 里拆成两个专用节点：

- `HIRTraitObjectCast`
- `HIRTraitObjectCall`

对应文件：

- `src/lona/sema/hir.hh`
- `src/lona/analyze/function.cc`

`cast[Trait dyn](&value)` 的分析规则是：

- 目标类型必须先被解析成 `DynTraitType`
- 源表达式必须显式写成 `&value`
- 被借用值必须可寻址
- concrete self type 必须有 visible impl
- 结果 trait object 会自动保留借用源的 pointee constness

`h.method()` 的分析规则是：

- 只有当 `h` 的类型已经是 `Trait dyn` 时，才会走 trait-object call 分支
- `Trait const dyn` 也会走同一条分支，但 setter 会在语义阶段被拒绝
- 普通 `value.method()` 仍然保持原有 struct / selector 语义
- 语义阶段会把调用绑定到 trait method slot index，而不是某个具体方法符号

## 7. LLVM Lowering：胖指针与 witness table

当前 LLVM lowering 采用外置 witness 模型，而不是 C++ 那种把 vptr 塞进对象：

- ordinary struct layout 保持不变
- trait object 内部表示是 `<erased_ptr, witness_ptr>`

更具体地说：

- `data_ptr` 指向原始 concrete object
- `witness_ptr` 指向当前 `(Trait, ConcreteType)` 的 witness table

在当前实现里，`Trait dyn` 的 LLVM 形状就是一个两字段匿名 struct：

```text
{ ptr, ptr }
```

字段顺序固定为：

1. `field[0] = data_ptr`
2. `field[1] = witness_ptr`

这也是为什么 lowering 代码总是先从 trait object 里取出：

- `trait.data`
- `trait.witness`

而不是像 C++ 对象那样先读对象头部的隐藏 vptr。

`emit/codegen.cc` 里会为每个可见 `(Trait, Type)` 组合生成一份内部 witness table：

- 符号名形如 `__lona_trait_witness__...`
- linkage 是 `InternalLinkage`
- LLVM 类型是 `[N x ptr]`
- `N` 等于 trait declaration 中的方法数
- 每个 slot 都是对应 concrete inherent method 的函数地址

可以把当前 witness table 近似理解成：

```text
@__lona_trait_witness__Trait__Type = internal constant [N x ptr] [
    ptr @Type.method0,
    ptr @Type.method1,
    ...
]
```

这里有两个实现边界需要明确：

- witness table 当前没有 header
  - 没有 size
  - 没有 align
  - 没有 drop glue
- 这不是遗漏，而是 trait v0 的直接结果
  - `Trait dyn` 是非 owning 借用视图
  - v0 不支持 owning trait object
  - v0 不支持需要额外 runtime metadata 的析构协议

### 7.1 slot 顺序

slot 顺序不是按名字排序，也不是按 lowering 时的偶然遍历顺序决定的。

当前实现使用的是 trait declaration 里的方法顺序：

- `TraitDecl.methods[0]` 对应 slot `0`
- `TraitDecl.methods[1]` 对应 slot `1`
- 以此类推

因此：

- witness table 初始化按 trait 方法声明顺序写入
- `h.method()` 的语义阶段会把方法解析成同一份 declaration 顺序里的 slot index

只要 trait declaration 本身稳定，构造端和调用端就会使用同一套 slot 编号。

### 7.2 slot 函数签名

虽然 witness table 里的元素在 LLVM 里统一存成 `ptr`，但调用时并不是“无类型乱调”。

语义阶段会先根据 trait method 生成 slot function type：

- 第一个参数总是擦除后的 receiver pointer
- 后续参数直接来自 trait 方法签名
- 返回类型直接来自 trait 方法签名

概念上，一个 trait 方法：

```text
def hash(x i32) i64
```

会对应到近似这样的 slot 签名：

```text
(ptr self, i32) -> i64
```

当前实现里：

- get-only receiver 会把这个 `self` 视为指向只读对象的擦除指针，也就是 `any const*`
- `set def` receiver 会把这个 `self` 视为可写擦除指针，也就是 `any*`
- witness slot 里仍然直接保存 concrete method symbol 地址
- 调用点会按“方法 ABI”发起调用，而不是把它当普通 free function

最后这点很重要。它保证了 native ABI 下那些特殊返回约定仍然成立，例如：

- 大结构体返回导致的 `sret`
- 隐式 `self` 与 `sret` 的固定相对顺序

也就是说，虽然 slot 里的函数指针被擦除成了 `ptr`，真正发起间接调用时仍然会恢复到 trait method 对应的 ABI 语义。

### 7.3 为什么当前不需要 adapter thunk

在 v0 当前实现里，slot 直接保存具体方法符号地址；setter / getter 的差别已经能用“擦除后 receiver 指针是否带 const”表达，所以还不需要为了 receiver mutability 再引入额外 adapter thunk。

这可以理解成当前 witness slot 依赖两条前提：

- trait 方法签名已经足够决定 slot 调用签名
- concrete inherent method 的 ABI 与这条 trait 签名兼容

如果未来支持：

- `set def` 的 dyn dispatch
- owning trait object
- 需要额外 metadata 的 runtime protocol

那就很可能要把“slot 里直接存真实方法地址”改成“slot 里存 adapter thunk 地址”。

## 8. `cast[Trait dyn](&value)` 的 IR 结果

这条路径不会分配内存，也不会复制对象。

lowering 的结果只是构造一个短生命周期或普通局部值：

- 第一个槽位是对象地址
- 第二个槽位是 witness table 地址

因此：

- `var x = Point(...)` 仍然只会创建 `Point`
- 只有显式 `cast[Hash dyn](&x)` 才会额外构造 trait object
- 当前 trait object 是非 owning 视图，不负责对象生命周期

## 9. `h.method()` 的 IR 结果

动态调用会被 lower 成 witness-slot 间接调用：

1. 从 trait object 里取出 witness pointer
2. 按 slot index 取出方法函数地址
3. 把 `data_ptr` 作为第一个实参，也就是擦除后的 receiver 传进去
4. 再拼接其它显式参数

因此动态路径的运行时成本明确包括：

- 一次 witness table 读取
- 一次间接调用

如果把当前流程写成伪代码，大致相当于：

```text
slot = witness[slot_index]
ret slot(data_ptr, arg0, arg1, ...)
```

其中：

- `slot_index` 来自 trait declaration 的方法顺序
- `data_ptr` 指向原始 concrete object
- `slot` 在调用前会按目标 trait method 的 slot 函数签名解释

这和静态 `Trait.method(&value, ...)` 的直接调用路径严格分开。

## 10. 与模块缓存 / artifact 复用的关系

trait v0 对缓存边界新增了两条必须牢记的约束：

1. trait / impl declaration 是 importer 可见接口的一部分
   - 所以必须进入 `ModuleInterface` 和 `interfaceHash`
2. 模块 artifact 现在已经区分 entry role
   - root 与 dependency 不能共用同一份入口产物

这两条约束合起来，才能避免这类错误：

- importer 继续复用旧 trait 方法签名
- importer 继续复用已经失效的 visible impl declaration
- 同一 session 中把 root 版本 artifact 错复用成 dependency 版本

## 11. 当前工程边界

如果你继续往后扩 trait，下面这些点会直接影响当前 lowering 设计：

- 一旦支持 owning trait object，就要重新定义存储位置和销毁语义
- 一旦支持泛型 / trait bound，就会碰到 monomorphization 和跨模块实例化策略

也就是说，当前实现是一个有意收口的 v0，而不是 future trait 系统的最终形态。
