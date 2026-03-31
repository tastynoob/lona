# Trait v0

> 这是一份面向 `lona` 的 trait 草案。
> 目标不是照搬某一门语言，而是在 `lona` 当前“结构体方法 + 显式指针 + 按模块产出 artifact”的实现基础上，先收口一版真正可落地的设计。

## 1. 范围

`trait_v0` 只定义四件事：

- `trait` 声明
- `impl Type: Trait` 声明
- concrete value 上的静态 trait 限定调用
- 显式、非 owning 的 `Trait dyn`

这份草案**不**试图在 v0 一次性解决：

- 泛型 / 模板
- associated type / associated const
- default method 全覆盖
- auto trait / negative impl
- owning trait object
- 隐式 heap boxing
- 生命周期系统

换句话说，这里讨论的是：

- 如何在没有泛型的前提下，把 trait 的静态能力和动态能力先收口

而不是：

- 如何一步到位得到 Rust 那样完整的泛型 trait 系统

## 2. 参考结论

这份草案主要参考了三类主流实现：

- C++ 的 virtual/vtable
- Rust 的 trait + `dyn Trait`
- Go 的 interface

最后推荐的组合是：

- 静态路径借 Rust 的“能力约束不是继承”
- 动态路径借 Rust/Go 的 `data + witness table`
- 明确不采用 C++ 那种“把 vptr 塞进每个对象”的主布局模型

### 2.1 C++ 给出的启发

C++ 的价值在于：

- 非动态路径仍然是 direct call
- vtable 本质就是一张按槽位组织的函数表
- 只有显式使用动态分发时才付额外成本

但它不适合直接照搬到 `lona`：

- 常见实现会把 vptr 进对象布局
- 多继承 / 虚基类 / thunk 调整复杂度很高
- `lona` 现在没有类继承树，这些复杂度几乎都是纯成本

对 `lona` 来说，C++ 最值得借的是：

- 只在显式动态路径付费

而不是：

- 继承驱动的对象布局模型

### 2.2 Rust 给出的启发

Rust 最接近 `lona` 想要的方向：

- trait 是 capability contract，不是继承
- `dyn Trait` 是显式动态分发
- dyn path 有单独的 object-safety 约束

`lona` v0 虽然先不做泛型，但仍然适合借 Rust 这两个思想：

- static 和 dynamic 明确分家
- trait object 是单独的用户显式选择

### 2.3 Go 给出的启发

Go interface 的优点是：

- 运行时模型直接
- interface value 本质上就是“对象地址 + 方法表”
- 跨包传递模型稳定

它不适合作为 `lona` 的主语义模板，因为：

- Go 的 interface 更偏动态抽象
- 它不是以“零成本静态路径”为设计中心

但它很适合给 `lona` 的 `Trait dyn` 提供运行时布局参考。

## 3. `lona` 当前约束

trait 设计不能脱离 `lona` 现状，否则会把实现难度判断错。

### 3.1 方法 lowering 已经存在

当前 `lona` 的结构体方法已经不是特殊 runtime 实体，而是普通函数加 hidden receiver：

- method 类型会把 receiver 变成第一个隐藏参数，见 [src/lona/declare/function.cc](../../src/lona/declare/function.cc)
- `StructType` 已经保存 method signatures，见 [src/lona/type/type.hh](../../src/lona/type/type.hh)
- imported module 的 method symbol 也会被接口物化成 external function，见 [src/lona/declare/interface.cc](../../src/lona/declare/interface.cc)

这意味着：

- trait 不需要重新发明“方法实体”的底层模型
- trait 最自然的做法是复用现有 method signature / method symbol / hidden receiver 约定

### 3.2 v0 先不做泛型

当前公开语法还没有用户级泛型/模板系统，参考：

- [type.md](../reference/language/type.md)
- [grammar.md](../reference/language/grammar.md)

因此这份草案的明确选择是：

- trait v0 先不依赖泛型
- trait 参数约束、跨模块 monomorphization、特化缓存都推迟到后续版本

### 3.3 当前默认编译模型是按模块产出 artifact

`lona` 目前是：

- 每个模块单独 lowering / codegen
- 每个模块单独缓存 bitcode / object artifact
- artifact 复用依赖 `sourceHash` / `interfaceHash` / `implementationHash` / dependency interface hash，见 [compiler_architecture.md](../internals/compiler/compiler_architecture.md)

这意味着：

- 如果 v0 一开始就引入跨模块特化型 trait 泛型，会直接撞上 artifact 缓存边界
- 先做“无泛型 trait + 显式 dyn object”是更稳的路线

### 3.4 `lona` 倾向显式指针，而不是隐式借用系统

当前语言已经有：

- 原始指针 / 可索引指针
- `ref` 绑定与参数
- shallow `const`

但没有：

- borrow checker
- lifetime inference
- 隐式所有权搬运

这意味着 `Trait dyn` v0 更适合设计成：

- 显式、非 owning 的借用视图
- 不隐藏 heap allocation

## 4. v0 的核心模型

### 4.1 concrete object 仍然只是 concrete object

如果某个结构体实现了某个 trait：

```lona
var x = Foo(args)
```

这里的 `x` 仍然只是 `Foo`。

它不会因为 `Foo` 实现了 trait，就自动变成带 vptr 的对象，也不会自动变成胖指针。

### 4.2 静态 trait 调用不需要 trait object

v0 允许在 concrete value 上显式写 trait 限定调用：

```lona
ret Hash.hash(x)
```

这里的语义是：

- 先验证 `Foo` 实现了 `Hash`
- 再把调用静态解析到 `Foo.hash`

这条路径不需要创建 trait object，也不需要 witness lookup。

因此它是 v0 里真正的“零额外运行时成本”路径。

### 4.3 动态 trait 只有在显式构造时才出现

如果用户需要运行时多态，才显式构造 `Trait dyn`。

例如概念上：

```lona
var obj = Foo(args)
var h Hash dyn = cast[Hash dyn](&obj)
ret h.hash()
```

这里建议直接复用 `lona` 已有的显式转换语法 `cast[T](expr)`，而不是再单独发明一套 `makeTrait[...]` / `dyn[...]` 工厂语法。

这样做的原因是：

- `lona` 已经有统一的显式转换入口 `cast[T](expr)`
- `lona` 已经有显式取地址 `&obj`
- `Trait dyn` 也可以像 `T const` 一样，写成后缀类型修饰形式

v0 语义必须保持一致：

- 从一个已存在对象构造
- 构造结果是非 owning 的 trait object
- 不发生隐式装箱

## 5. 建议语法

### 5.1 trait 声明

```lona
trait Hash {
    def hash() u64
}

trait CounterLike {
    def read() i32
    set def inc(step i32) i32
}
```

规则建议是：

- `trait` 是新的顶层声明
- trait body 里的成员当前只允许方法签名
- `def` 表示只读 receiver 需求
- `set def` 表示可写 receiver 需求
- v0 不引入字段、associated type、associated const

receiver 语义直接复用现有 struct method 规则：

- `def foo()` 等价于要求实现端提供 `Self const*` receiver 的方法
- `set def foo()` 等价于要求实现端提供 `Self*` receiver 的方法

### 5.2 impl 声明

建议 v0 先采用**标记式 impl**：

```lona
struct Point {
    x i32
    y i32

    def hash() u64 {
        ret cast[u64](self.x) ^ cast[u64](self.y)
    }
}

impl Point: Hash
```

含义是：

- `impl Point: Hash` 本身不承载新方法体
- 它只声明“`Point` 满足 `Hash`”
- 编译器验证 `Point` 是否已经有与 `Hash` 要求一致的 inherent method

这样做的好处是：

- 完全复用现有 struct method lowering
- 不需要立刻引入一套新的 impl-body AST / codegen 路径
- trait impl 在接口层只需要记录 `(trait, self-type)` 以及槽位对应的方法符号

future 才再考虑：

```lona
impl Point: Hash {
    def hash() u64 {
        ret ...
    }
}
```

### 5.3 trait 限定调用

为了避免 v0 一开始就把 trait method 注入普通 dot lookup，建议提供显式限定调用：

```lona
ret Hash.hash(p)
```

这条语法的作用是：

- 在普通 concrete value 上也能使用 trait 方法
- 不依赖复杂的全局 trait method 注入查找
- 给后续歧义场景保留稳定写法

v0 建议规则：

- `p.hash()` 仍优先只看 inherent method
- trait method 不自动进入普通 concrete value 的成员查找
- 想调 trait method，就显式写 `Trait.method(p, ...)`

### 5.4 Trait dyn

建议 v0 允许显式写：

```lona
var obj = Foo(args)
var h Hash dyn = cast[Hash dyn](&obj)
```

规则建议是：

- `Trait dyn` 是新的用户可见类型
- 它表示一个非 owning 的 trait object
- 构造时必须显式从已有对象出发
- v0 不支持对临时值隐式构造 trait object

这里推荐把 `dyn` 设计成**后缀类型修饰符**，而不是 Rust 风格的前缀关键字：

- `Hash dyn`
- `CounterLike dyn`

这样更接近 `lona` 当前的类型书写风格：

- `T const`
- `T*`
- `T[*]`

也就是说，下面这种思路在 v0 不建议直接支持：

```lona
var h Hash dyn = cast[Hash dyn](Foo(args))
```

因为它会立刻碰到：

- 对象存放位置
- 临时值生命周期
- 销毁语义
- 是否需要隐式 boxing

这些都不应该混进第一版 trait。

## 6. trait object 的布局

### 6.1 用户可见语义

`Hash dyn` 是统一的抽象类型。

无论底层实现者是：

- `Foo`
- `Bar`
- `Baz`

只要它们都实现 `Hash`，构造出来的动态 trait object 都应该是同一个用户类型：`Hash dyn`。

### 6.2 推荐 lowering

推荐内部 lowering 为：

- `erased_data_ptr`
- `witness_ptr`

也就是概念上的：

```text
struct DynHash {
    void* data;
    HashWitness* witness;
}
```

其中：

- `data` 指向真实对象
- `witness` 指向 `(Hash, ConcreteType)` 对应的 witness table

### 6.3 为什么不是 `<Foo, witness_ptr>`

`<Foo, witness_ptr>` 可以作为编译器内部的某种“带见证的具体值”思路，但它不适合作为用户层 trait object。

原因是：

- `<Foo, witness_ptr>` 和 `<Bar, witness_ptr>` 不是同一个类型
- 它们不能统一承载运行时多态
- 用户想要的 `Hash dyn` 必须是统一布局、统一类型

所以 v0 应该明确：

- 用户可见的 trait object 是 `Trait dyn`
- 它的内部布局是 `<erased_ptr, witness_ptr>`
- `<ConcreteType, witness_ptr>` 不作为用户层稳定语义

### 6.4 为什么 v0 不单独做 `makeTraitPtr`

从语义上说，`Trait dyn` 本身已经是“指向对象的借用型胖指针”。

如果后续需要支持从原始指针构造，也应该理解为：

- 仍然构造同一个 `Trait dyn`
- 只是 `data_ptr` 的来源不是 `ref obj`，而是某个显式指针

例如概念上：

```lona
var p Foo* = &obj
var h Hash dyn = cast[Hash dyn](p)
```

而不是再额外发明一个和 `Trait dyn` 平行的新对象类别。

## 7. 语义规则

### 7.1 trait 成员匹配

对 `impl Type: Trait`，建议先要求：

- 方法名一致
- 参数个数一致
- 参数类型一致
- 返回类型一致
- receiver access 一致

也就是说：

- `trait` 里的 `def foo()` 只能由 `Type` 的普通 `def foo()` 满足
- `trait` 里的 `set def foo()` 只能由 `Type` 的 `set def foo()` 满足

这样规则最清晰，也最容易复用现有 method type。

### 7.2 coherence 与 orphan rule

必须有 coherence，不然 import graph 一大就会出现同一 `(Trait, Type)` 多重实现。

推荐规则：

- 全程序内，同一个 `(Trait, Type)` 最多只能有一个 impl
- 一个 impl 合法，当且仅当以下条件至少一个成立：
  - trait 定义在当前模块
  - self type 定义在当前模块

这就是 Rust 风格 orphan rule 的简化版。

### 7.3 dyn compatibility

不是所有 trait 都应该自动支持 dynamic dispatch。

建议 v0 / future `Trait dyn` 只允许 dyn-compatible trait：

- 成员必须都是方法
- 方法签名中不能在 receiver 之外出现裸 `Self`
- 不允许需要按值移动未知大小 `Self` 的接口

由于 v0 先不做泛型和 associated item，这套规则可以保持很简单。

### 7.4 模块接口可见性

trait 声明和 impl 头部都需要进入模块接口。

原因是 importer 需要知道：

- 某个 trait 是否存在
- 某个 type 是否实现了某个 trait
- 某个 trait slot 最终对应哪个方法符号

因此 v0 至少需要给 `ModuleInterface` 增加：

- `TraitDecl`
- `TraitImplDecl`

同时：

- trait declaration 变化应计入 `interfaceHash`
- 可见 impl header 变化也应计入 `interfaceHash`

因为这些变化会直接影响下游 name resolution 和 trait satisfaction。

## 8. 推荐实现模型

### 8.1 静态路径

对下面的代码：

```lona
var p = Point(x = 1, y = 2)
ret cast[i32](Hash.hash(p))
```

推荐 lowering 是：

- 语义阶段验证 `Point` 实现了 `Hash`
- 解析 `Hash.hash(p)` 到具体方法符号 `Point.hash`
- codegen 直接输出普通 direct call

这样这条路径的运行时成本只剩：

- 参数传递
- 普通函数调用

它不需要：

- trait object 构造
- witness lookup
- 间接跳转

这就是 v0 的零额外抽象成本路径。

### 8.2 动态路径

对下面的代码：

```lona
var obj = Point(x = 1, y = 2)
var h Hash dyn = cast[Hash dyn](&obj)
ret cast[i32](h.hash())
```

推荐 lowering 是：

- 生成 `(Hash, Point)` 对应的 witness table
- 生成必要的 adapter thunk
- `cast[Hash dyn](&obj)` 构造 `<erased_ptr, witness_ptr>`
- `h.hash()` 走 witness slot 的间接调用

这条路径有明确的运行时成本：

- 一个两字长对象
- 一次间接调用

但它只在用户显式选择 `Trait dyn` 时才发生。

### 8.3 adapter thunk

witness table 不一定直接存 inherent method 本体地址，也可以存 adapter thunk：

- 把 `void*` / `void const*` cast 回具体 `Self*`
- 再调用真实的 `Type.method`

这样做的好处是：

- witness table 槽位签名固定
- concrete method ABI 可以继续保持当前 hidden receiver 约定

## 9. 编译器改动建议

### 9.1 parser / AST

新增：

- `trait` 顶层声明
- `impl Type: Trait` 顶层声明
- `Trait dyn` 类型语法
- `cast[Trait dyn](expr)` 这种 trait object 构造

### 9.2 接口收集

当前 `ModuleInterface` 只有：

- type
- function
- global

trait 需要扩成：

- trait declaration
- trait impl header

建议 trait 进入独立接口表，而不是假装它是普通 type。

### 9.3 resolve / analyze

需要增加：

- trait name lookup
- impl satisfaction 检查
- `Trait.method(value, ...)` 这种显式限定调用解析
- dyn-compatible 诊断
- `Trait dyn` 构造与方法调用解析

### 9.4 codegen

静态路径：

- trait-qualified call 直接绑定到具体方法符号

动态路径：

- 生成 witness table 常量
- 生成必要的 adapter thunk
- trait object method call 走间接调用

### 9.5 缓存与 hash

这版 v0 的好处是没有引入跨模块 trait 特化实例，因此不会直接冲击当前 artifact 缓存模型。

建议：

- trait declaration / visible impl header 进入 `interfaceHash`
- witness table / adapter thunk / trait object lowering 进入 `implementationHash`

## 10. 推荐分阶段落地

### 10.1 Phase 1：真正可落地的 MVP

建议第一阶段只做：

- `trait` 声明
- 标记式 `impl Type: Trait`
- `Trait.method(value, ...)` 显式限定调用
- `Trait dyn`
- `cast[Trait dyn](&value)` 这种显式借用构造
- witness table
- dyn-compatible 诊断

这阶段的特点是：

- 有真实的静态零额外成本路径
- 有显式动态多态路径
- 不要求改 struct 布局
- 不要求泛型
- 不要求完整 generic export / instance cache
- 实现复杂度可控

### 10.2 Phase 2：trait impl body / richer syntax

再做：

- `impl Type: Trait { ... }`
- trait declaration 自带 default method
- 更完整的 trait-qualified method lookup 体验

### 10.3 Phase 3：泛型与特化

最后再做：

- trait 参数约束
- named generic parameter
- monomorphization
- 跨模块特化缓存
- associated type / associated const

这一阶段才接近 Rust/C++ 的完整 trait + 泛型能力。

## 11. 我对 v0 的明确建议

如果只选一条最稳的路，我建议：

1. v0 先明确不做泛型。
2. v0 的 impl 先做成标记式，只绑定已有 inherent method，不允许 impl body。
3. 静态 trait 调用先只支持 `Trait.method(x)` 这种限定写法。
4. `Trait dyn` 只做成非 owning 的借用型胖指针。
5. `Trait dyn` 的内部布局固定为 `<erased_ptr, witness_ptr>`，不要把 `<ConcreteType, witness_ptr>` 暴露成用户语义。
6. 不要把 vptr 塞进普通对象布局。
7. 不要在 v0 支持从临时值直接构造 owning trait object。

这版设计最符合 `lona` 当前实现：

- 它复用了现有 method lowering
- 不破坏对象布局
- 不会和当前模块 artifact 缓存正面冲突
- 既保留了静态零额外成本路径，也给出了清晰的动态路径

## 12. 仍需你拍板的开放点

- `trait` 是否进入独立命名空间，还是挂进现有 type namespace
- `Trait.method(x)` 是否就是 v0 唯一稳定的 trait-qualified call 语法
- `impl Type: Trait` 是否作为 v0 最终稳定的实现声明语法
- `Trait dyn` 是否作为 v0 最终稳定的 trait object 类型写法
- `cast[Trait dyn](&x)` 是否作为 v0 最终稳定的构造语法
- v0 是否允许从显式原始指针构造 `Trait dyn`
- v0 是否允许 trait declaration 自带 default method body
- 何时引入可见性规则，以支持未来“可导出的 trait-bound API”

## 参考资料

- Rust Reference, Trait objects: https://doc.rust-lang.org/reference/types/trait-object.html
- Go Language Specification, Interface types: https://go.dev/ref/spec
- Go runtime source, interface/runtime support: https://go.dev/src/runtime/iface.go
- Go internal ABI source, interface headers: https://go.dev/src/internal/abi/iface.go
- Itanium C++ ABI, Virtual Table Layout: https://itanium-cxx-abi.github.io/cxx-abi/abi.html#vtable
