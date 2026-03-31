# Generic v0

> 这是一份面向 `lona` 的泛型草案。
> 目标不是照搬某一门语言，而是在 `lona` 当前“显式类型后缀 + 显式指针 + 按模块产出 artifact + trait v0 已落地”的基础上，先收口一版真正可实现、能和 trait v0 配合工作的泛型系统。

## 1. 范围

`generic_v0` 建议先定义这几件事：

- 泛型 `struct`
- 泛型顶层函数
- 泛型 `impl`
- 基于 trait 的泛型约束
- 受限的 `any` 擦除类型，用于显式类型擦除边界
- 按需实例化的静态泛型代码生成

这份草案**不**试图在 v0 一次性解决：

- 泛型 trait
- associated type / associated const
- const generic
- 值级模板参数
- partial specialization
- overload set + 模板偏序
- 默认泛型参数
- HKT
- 依赖类型
- bare `any` 的按值语义

换句话说，这里讨论的是：

- 如何让 `lona` 得到一版和 trait v0 能互相配合的静态泛型系统

而不是：

- 如何一步到位得到 Rust + C++ + Go 的全部泛型能力总和

## 2. 参考结论

这份草案主要参考三类主流实现：

- C++ 模板
- Rust 泛型 + trait bound
- Go 泛型 + constraint

最后推荐的组合是：

- 表面语法更接近 Go：`Name[T]`
- 语义模型更接近 Rust：typed generic + trait bound + monomorphization
- 明确不采用 C++ 那种“文本级模板替换 + 晚期报错”的主模型

### 2.1 C++ 给出的启发

C++ 模板的价值在于：

- 静态抽象能力非常强
- 实例化后的代码可以做到零额外运行时成本
- 泛型 struct / function / algorithm 的覆盖面极大

但它不适合直接照搬到 `lona`：

- 模板本质上更接近“编译期文本/语法驱动的特化系统”
- diagnostics 往往晚且噪声大
- overload resolution、partial specialization、SFINAE 会迅速把语义面复杂度抬高
- 这套模型和 `lona` 当前相对直接的前端/接口哈希架构不匹配

对 `lona` 来说，C++ 最值得借的是：

- 静态实例化后的零额外成本

而不是：

- 文本级模板替换语义

### 2.2 Rust 给出的启发

Rust 给出的方向最接近 `lona` 需要的 v0：

- 泛型是 typed 的，不是文本替换
- trait bound 是显式的能力约束
- 实例化后仍然走静态 direct call
- dynamic dispatch 通过 `dyn Trait` 单独表达，不和静态泛型混在一起

对 `lona` 来说，Rust 最值得借的是：

- generics 和 traits 的分层模型
- 静态路径与动态路径分家
- 以 monomorphization 作为默认代码生成模型

### 2.3 Go 给出的启发

Go 泛型的优点是：

- 用户表面语法比较收敛
- `Name[T]` 这类写法直接、清楚
- constraint 写法也更偏接口能力，而不是模板元编程

它不适合作为 `lona` 的主实现模板，因为：

- Go 的实现策略不以“所有静态路径都极致零成本”为首要目标
- Go interface / constraint 的整体系统和 `lona` 当前 trait v0 并不等价

但它很适合给 `lona` 的泛型表面语法提供参考。

### 2.4 推荐组合

因此更适合 `lona` 的 v0 组合是：

- 语法：借 Go 的 `Name[T]`
- 语义：借 Rust 的 typed generic + trait bound
- 代码生成：默认走 Rust 风格 monomorphization
- 明确排除 C++ 模板那套文本替换与偏特化模型

## 3. `lona` 当前约束

泛型设计不能脱离当前编译器架构，否则会把实现难度判断错。

### 3.1 trait v0 已经落地，但还是“收口版”

当前 `lona` 已经有：

- `trait`
- `impl Type: Trait`
- `Trait.method(value, ...)`
- `Trait dyn`

但还没有：

- 泛型 trait
- associated item
- default method 全覆盖

这意味着泛型 v0 必须优先和**现有** trait v0 配合，而不是假设未来 trait 系统已经完整。

### 3.2 当前编译模型是“按模块缓存 artifact”

当前架构是：

- 每个模块单独收集接口
- 每个模块单独 lowering / codegen
- artifact 复用依赖 `interfaceHash` / `implementationHash`

这点很关键。

因为一旦引入泛型，就会立刻碰到：

- 实例化代码放在哪个模块生成
- exporter 和 importer 谁拥有实例化产物
- `interfaceHash` 需要暴露到什么粒度

所以泛型 v0 不能只讨论语法，必须从一开始就把实例化和缓存模型写清楚。

### 3.3 `lona` 已经有显式 `const`、指针和 trait dyn

这意味着：

- 泛型参数要能和 postfix 类型修饰语法组合
- 泛型不能偷偷引入新的隐藏借用语义
- 类型擦除边界要继续保留 pointee constness
- static generic 与 `Trait dyn` 必须继续分家

换句话说：

- `T: Hash` 是静态能力约束
- `Hash dyn` 是运行时 trait object

这两条路径在 v0 中不能混成同一种东西。

### 3.4 `lona` 当前语法里 `<...>` 已经很忙

当前 `<...>` 已经用于：

- tuple type
- 函数显式取指针的参数列表

因此泛型如果也复用 `<...>`，会让现有语法更容易冲突。

这也是为什么泛型 v0 更适合优先考虑：

- `Name[T]`

而不是：

- `Name<T>`

## 4. v0 的核心模型

### 4.1 泛型是“类型参数化”，不是模板文本替换

`generic_v0` 应该是 typed generic：

- 泛型参数首先是类型系统中的参数
- 约束首先是语义阶段检查
- 实例化之后才进入具体布局和具体代码生成

而不是：

- 先把语法片段替换出来，再期待后续碰运气通过 type check

这条原则会直接影响：

- diagnostics 质量
- `interfaceHash` 边界
- trait bound 的实现方式

### 4.2 v0 默认走静态实例化

这版泛型应该默认是静态路径：

- generic struct 实例化后得到具体布局
- generic function 实例化后得到具体函数体
- 有 trait bound 的 generic function 在实例化后仍然是 direct call

也就是说：

- `T: Hash` 不是 runtime dictionary/object
- `T: Hash` 不是 `Hash dyn`
- `T: Hash` 的本质是“实例化前的能力约束”

### 4.3 `Trait dyn` 仍然保留为显式动态路径

trait 和泛型在 v0 中应明确分层：

- `def hash_all[T: Hash](x T) i32`
  - 静态泛型路径
  - 实例化后 direct call
- `def hash_dyn(x Hash dyn) i32`
  - 显式动态路径
  - witness lookup + indirect call

这和当前 trait v0 的方向完全一致：

- static path 零额外成本
- dynamic path 显式付费

### 4.4 v0 不把 generic 当“任意元编程”

这版只讨论：

- 类型参数
- trait 约束
- 实例化

明确不讨论：

- 编译期值运算
- `if constexpr`
- 类型函数
- 模板偏特化

### 4.5 `any` 是受限的擦除类型，不是顶层万能类型

这版草案建议同时引入一个**用户可见但严格受限**的 `any`：

- `any` 不是 Go 的 `any`
- `any` 不是 TypeScript 的 `any`
- `any` 不是“可以做任何事”的顶层类型

它更接近：

- C 的 `void`
- Rust 运行时边界常见的 `*const ()`
- Zig 的 `anyopaque`

更具体地说，`any` 的角色是：

- 表示“具体 pointee type 已经被擦除”
- 让用户和编译器都能显式看见“这里存在一个 erased pointer”

### 4.6 `any` 只允许出现在指针位置

v0 建议只允许：

- `any*`
- `any const*`
- `any[*]`
- `any const[*]`

明确不支持：

- `var x any`
- `def f() any`
- `struct S { value any }`
- tuple / array / generic argument 中按值携带 bare `any`

原因很直接：

- bare `any` 没有确定大小
- bare `any` 没有确定对齐
- bare `any` 没有确定拷贝/销毁语义

所以 `any` 在 v0 里必须被理解成：

- 一个只能通过 pointer form 使用的 opaque pointee type

### 4.7 `any` 的核心用途是“擦除边界”，不是 generic 源码层主语言

generic v0 的 typed 源码层仍然应该使用：

- `T`
- `U`
- `Box[T]`
- `T: Hash`

而不是让用户在正常 generic body 里改用 `any` 编程。

例如：

```lona
def id[T](value T) T {
    ret value
}
```

这里仍然应该在源码层写 `T`，而不是：

```lona
def id(value any) any
```

`any` 更适合出现的地方是：

- compiler-generated erased helper
- trait witness / adapter thunk 边界
- 用户显式想表达“我手里只有一个 erased pointer”

### 4.8 `any` 保留 pointee constness

这点很重要。

类型擦除不会抹掉 pointee 的 `const` 资格：

- `T* -> any*`
- `T const* -> any const*`

也就是说，擦除后的 pointer 仍然保留“这是不是只读借用”的信息。

这和当前 trait v0 dyn mutability 方向是一致的：

- `data_ptr` 若来自可写借用，则是 `any*`
- `data_ptr` 若来自只读借用，则是 `any const*`

后续能不能走 setter / writable path，主要由这个擦除后 pointer 的 constness 决定。

## 5. 建议语法

### 5.1 泛型参数列表

建议统一使用方括号：

```lona
struct Box[T] {
    value T
}

def id[T](value T) T {
    ret value
}
```

理由：

- 与 `cast[T](expr)` / `sizeof[T]()` 一致，都把 `[]` 用作“类型参数入口”
- 避开当前 `<...>` 在 tuple / 函数取指针上的既有用途
- 比 `foo<T>` 更不容易和现有表达式语法冲突

### 5.2 泛型类型实参

类型位置建议写作：

```lona
var a Box[i32]
var b Pair[i32, bool]
var c Result[Point, Error]
```

构造也沿用同一写法：

```lona
var box = Box[i32](value = 1)
```

### 5.3 泛型函数调用

v0 建议支持两种路径：

1. 实参推断：

```lona
var a = id(1)
```

2. 必要时显式写类型参数：

```lona
var b = id[i32](1)
```

但这条显式函数类型参数路径要有一个边界：

- 只对 `dot-like name` 直接开放，例如 `id[i32](...)`、`pkg.id[i32](...)`
- 不把它扩展成“任意表达式后都能接 `[T]`”

否则会很容易和索引表达式混起来。

### 5.4 泛型约束

建议先使用和现有 `impl Type: Trait` 一致的 `:` 风格：

```lona
def hash_one[T: Hash](value T) i32 {
    ret Hash.hash(value)
}
```

多个参数：

```lona
def pair_eq[A: Eq, B: Eq](left Pair[A, B], right Pair[A, B]) bool {
    ret Eq.eq(left.first, right.first) && Eq.eq(left.second, right.second)
}
```

多个 bound 的语法，建议 v0 明确但保持简单：

```lona
def use_both[T: Hash + Eq](value T) i32 {
    if Eq.eq(value, value) {
        ret Hash.hash(value)
    }
    ret 0
}
```

这里的 `+` 只在 generic bound list 中表示“同时满足多个 trait”，不扩展成一般类型运算。

### 5.5 泛型 impl

因为 trait 和泛型是一体设计，所以 v0 建议直接支持泛型 impl：

```lona
struct Box[T: Hash] {
    value T

    def hash() i32 {
        ret Hash.hash(self.value)
    }
}

impl[T: Hash] Box[T]: Hash
```

但这里有个必须收紧的点：

- 语法层允许 `impl[Params] Type[Args]: Trait`
- 语义层仍然保持和 trait v0 一样的“header-only impl”
- 不在 generic v0 中顺便引入 `impl { ... }` body

### 5.6 v0 暂不支持的语法

这版建议明确不支持：

- `trait Iterator[T]`
- `struct Array[T, N]`
- `def foo[T = i32](...)`
- `impl[T] Foo[T] { ... }` 中新增 generic method body 规则
- method 自己再带独立的泛型参数，例如 `def map[U](...)`

其中最后一条不是理论上不行，而是 v0 为了收 scope 建议暂缓。

### 5.7 `any` 的建议语法

如果引入 `any`，建议它严格复用现有 pointer / const 后缀体系：

```lona
var p any*
var q any const*
var bytes any[*]
var readonly_bytes any const[*]
```

显式转换也沿用统一的 `cast[T](expr)`：

```lona
var erased any* = cast[any*](ptr)
var restored Point* = cast[Point*](erased)
```

这里必须明确：

- 这类恢复 cast 是显式的
- 编译器不能自动证明它安全
- 它表达的是“调用者知道这个 erased pointer 原本指向什么 concrete type”

## 6. 语义规则

### 6.1 泛型参数是 nominal type parameter

在 v0 中，`T`、`U` 这些参数代表：

- 一个未定 concrete type

它们不是：

- duck type
- 结构展开规则
- 编译期文本替换孔洞

### 6.2 约束通过 visible trait impl 满足

如果写：

```lona
def hash_one[T: Hash](value T) i32
```

那么语义要求是：

- 实例化点上的 concrete type 必须有 visible `impl Type: Hash`

这意味着泛型和 trait 的真正接点就是：

- trait v0 的 impl satisfaction
- trait v0 的 visible impl graph

而不是新的独立 constraint 系统。

### 6.3 约束内的方法调用仍用 trait 限定调用

在 generic body 内，不建议因为有 bound 就自动开放 `value.hash()`。

建议继续复用 trait v0 已经定下来的规则：

```lona
ret Hash.hash(value)
```

这样做的好处是：

- 不需要再额外设计 bound 注入的 method lookup
- 泛型不会把普通 dot lookup 弄复杂
- 和现有 trait v0 规则保持一致

### 6.4 类型推断保持保守

generic function call 的 v0 推断建议只从这些来源做：

- 显式 type args
- 实际参数类型

不建议 v0 就依赖：

- 返回类型期望
- 赋值左值类型
- 复杂的双向推断

原因是：

- 这样 diagnostics 更稳定
- 更容易避免“推断成功但解释路径不透明”
- 对当前编译器前端更现实

### 6.5 不允许未约束地做需要布局/行为的操作

如果一个 generic body 里写：

```lona
def foo[T](value T) i32 {
    ret value.hash()
}
```

那应该被拒绝。

如果没有：

- `T: SomeTrait`
- 或其它明确语义保证

就不能假设 `T` 有某个方法、字段、布局或运算能力。

### 6.6 specialization 不进 v0

v0 明确不支持：

- 函数特化
- impl 特化
- 对同一 generic item 按某些 type args 走不同 body

原因是：

- 它会立刻把 coherence、实例化选择和缓存边界变复杂
- trait v0 本身都还没有走到 associated type / generic trait 那一层

### 6.7 `any` 不是 trait object，也不是 generic bound

`any` 必须和下面两样东西明确区分：

1. `Trait dyn`
   - `Trait dyn` 是 `(data_ptr, witness_ptr)` 这种带行为表的显式动态对象
   - `any*` / `any const*` 只是“擦除了 pointee type 的 pointer”
   - `any` 自己不携带方法集

2. `T: Trait`
   - `T: Trait` 是静态 generic bound
   - 它要求实例化点存在 visible impl
   - 它不是 erased pointer

因此下面这些不能混为一谈：

- `any*`
- `Hash dyn`
- `T: Hash`

### 6.8 从 `any*` 恢复具体类型必须显式 cast

从 `any*` / `any const*` 回到 concrete pointer，例如：

```lona
cast[Point*](p)
cast[Point const*](p)
```

都必须是显式操作。

v0 建议把它视为“用户承担正确性责任的低层能力”：

- 编译器只检查 pointer / constness 形状是否匹配
- 不尝试证明 erased source 的真实 concrete identity

也就是说，这更接近：

- 受控的 runtime boundary cast

而不是：

- 一条总是静态安全的普通类型转换

### 6.9 如果某条路径需要 erased self，应使用 `any*` / `any const*`

你可以把 `any` 理解成“运行时边界上的 erased self/self-like parameter”。

例如未来如果某条内部 lowering 路径需要一个已经擦除了具体类型、但仍然保留可写性的 receiver，它应当长成：

- `any*`
- 或 `any const*`

而不是：

- `Trait dyn`
- 或 bare `any`

这样做的好处是：

- erased receiver 语义非常直接
- constness 仍然可追踪
- 不会把“只是擦除类型”误说成“已经拥有某个动态方法集”

## 7. 代码生成模型

### 7.1 推荐默认使用 monomorphization

v0 默认建议：

- generic struct 按 concrete type args 生成具体布局
- generic function 按 concrete type args 生成具体符号

也就是 Rust 风格的按需实例化。

这样做的直接好处是：

- 性能模型清晰
- trait bound 的静态调用可以继续 direct call
- 不需要为每个泛型参数引入运行时 dictionary

### 7.2 generic struct 的实例化结果

例如：

```lona
struct Box[T] {
    value T
}
```

实例化后：

- `Box[i32]` 是一个具体 struct layout
- `Box[Point]` 是另一个具体 struct layout

它们不是同一个 runtime type。

### 7.3 generic function 的实例化结果

例如：

```lona
def id[T](value T) T {
    ret value
}
```

实例化后可以近似理解成：

- `id[i32]`
- `id[Point]`

各自有自己的 concrete function symbol。

### 7.4 trait bound 仍然走静态路径

例如：

```lona
def hash_one[T: Hash](value T) i32 {
    ret Hash.hash(value)
}
```

对 `Point` 实例化之后，建议 lowering 等价于：

- 验证 `impl Point: Hash`
- 解析 `Hash.hash(value)` 到 `Point.hash`
- 直接输出 direct call

也就是说：

- `T: Hash` 不等于 `Hash dyn`
- 有 bound 的泛型函数仍然应保持零额外运行时成本

### 7.5 `Trait dyn` 继续作为显式动态逃生口

generic v0 不应该试图把所有“无法静态实例化的情况”自动转成 `Trait dyn`。

例如：

- `T: Hash` 是静态泛型
- `Hash dyn` 是显式动态 trait object

用户如果要动态路径，应该自己明确写出来。

### 7.6 `any` 主要服务于擦除边界，不替代 monomorphization

即使引入 `any`，generic v0 的主代码生成模型也不应改变：

- generic struct 继续实例化成具体布局
- generic function 继续实例化成具体函数
- 有 trait bound 的 generic static path 继续 direct call

`any` 只适合出现在这些边界上：

- compiler-generated erased helper
- trait/object adapter thunk
- 用户显式的低层 erased pointer API

不应该把它扩展成：

- “generic function body 一律先擦除成 `any*` 再解释执行”
- “有 bound 的泛型都靠 `any + witness` 实现”

否则就会把这版 generic v0 从“静态泛型”重新拉回“半动态类型擦除系统”。

## 8. 模块接口、缓存与实例化边界

这是 `lona` generic v0 最需要提前说清楚的部分。

### 8.1 为什么不能只把泛型当普通 declaration

当前非泛型导出通常只需要 importer 看见：

- 签名
- symbol 名

但 generic function 不够。

因为 importer 如果想实例化：

- 只知道签名是不够的
- 还需要一个“可实例化的 body 模板”

因此 generic v0 的模块接口不能只暴露：

- `def map[T, U](...)`

还必须能让 importer 获取某种稳定的 instantiation template。

### 8.2 推荐模型：接口暴露 generic signature，模板单独缓存

更适合 `lona` 当前架构的做法是：

1. `ModuleInterface` 暴露：
   - generic item 的名字
   - type parameter 列表
   - bound 列表
   - 签名
2. 编译缓存层额外保存：
   - generic body 的可实例化模板
   - 推荐是 typed HIR / lowered template，而不是源代码文本
3. importer 看到具体 type args 后：
   - 在本模块或专门的实例化单元里 materialize 一份 concrete instance

这里推荐的是“语义模板导出”，不是 C++ 那种头文件文本展开。

### 8.3 instance key

无论实例化放在 importer 还是单独实例化单元，都需要稳定 key。

建议 instance key 至少包含：

- 定义模块 key
- generic item identity
- concrete type args
- 相关 interface hash
- 若有 trait bound，则还要包含可见 impl graph 版本

否则很容易出现：

- 旧实例化结果误复用
- trait impl 变化后泛型代码不失效

### 8.4 v0 更推荐“实例化归使用点”

对当前 `lona` 架构，我更推荐：

- generic instance 归使用点模块生成

而不是：

- 先让定义模块预生成所有可能实例

原因是：

- 后者在当前模块化架构里几乎不可控
- 使用点更知道自己到底需要哪些 concrete type args
- 这更接近 Rust/C++ 的现实使用方式

代价是：

- importer 需要拿到可实例化模板
- instance cache 设计要更认真

但这是比“跨模块集中实例化调度器”更现实的 v0 路线。

## 9. 与 trait v0 的关系

### 9.1 为什么说 trait 和 generics 是相辅相成的

如果没有泛型：

- trait v0 只能提供 concrete type 上的 static qualified call
- 或显式 `Trait dyn`

如果没有 trait：

- 泛型 v0 只能做“完全无约束的类型参数替换”
- 很难表达真实算法需要的能力边界

两者合起来，才构成完整的静态抽象模型：

- generics 负责参数化
- trait 负责能力约束

### 9.2 建议的组合方式

建议 v0 组合方式是：

- trait v0 提供：
  - `trait`
  - `impl`
  - `Trait.method(...)`
  - `Trait dyn`
- generic v0 提供：
  - `struct Foo[T]`
  - `def foo[T: Trait](...)`
  - `impl[T: Trait] Foo[T]: OtherTrait`

这就足以覆盖很大一类零成本抽象场景。

### 9.3 v0 暂不做 generic trait

例如下面这种：

```lona
trait Iterator[T] {
    def next() Option[T]
}
```

建议不进入 v0。

因为它会立刻牵出：

- associated type 的替代设计
- generic trait impl 的一致性
- 更复杂的 bound 求解

对当前 `lona` 来说，这一层太早。

## 10. 推荐的分阶段落地

### 10.1 Phase 1：泛型表面语法与类型参数

先做：

- `struct Foo[T]`
- `def foo[T](...)`
- `Foo[i32]`
- `foo[i32](...)`
- 最基本的 type argument 检查

### 10.2 Phase 2：trait bound 与 generic impl

再做：

- `[T: Hash]`
- `[T: Hash + Eq]`
- `impl[T: Hash] Box[T]: Hash`
- generic body 内的 trait-qualified static call

### 10.3 Phase 3：实例化缓存与跨模块模板导出

最后做：

- generic template cache
- interface hash / impl graph 驱动的 instance invalidation
- importer 侧实例化

只有这阶段也做完，generic v0 才算真正和当前 `lona` 的模块化编译模型接上。

## 11. 我对 v0 的明确建议

如果只选一条最稳的路，我建议：

1. 表面语法用 `Name[T]`，不要用 `Name<T>`。
2. v0 只做类型参数，不做 const generic / 值参数。
3. v0 默认使用 monomorphization，不做 runtime dictionary 作为主路径。
4. 泛型约束直接复用 trait v0，不另造一套 constraint 语言。
5. bound 内的方法调用继续写 `Trait.method(value)`，不要自动注入 `value.method()`。
6. 必须从一开始就把 generic template 的接口导出与缓存边界设计进去。
7. v0 不做 specialization、generic trait、associated type。

这套组合最符合 `lona` 当前形态：

- 语法能和现有 `cast[T]` / `sizeof[T]` / postfix type 风格协调
- 语义能直接接上 trait v0
- 性能模型保持静态零成本
- 不会在第一版就掉进 C++ 模板式复杂度

## 12. 仍需拍板的开放点

- 泛型方法 `def map[U](...)` 是否进入 v0，还是只支持 generic top-level `def`
- generic instance 具体放在 importer 模块，还是单独的实例化 artifact bucket
- 多个 trait bound 是否直接使用 `+`，还是首版只允许单 bound，再把多 bound 延后
- struct 的 type parameter 是否允许在声明处直接写 bound，例如 `struct Box[T: Hash]`

## 参考资料

- Rust Reference, Generics: https://doc.rust-lang.org/reference/items/generics.html
- Rustc Dev Guide, Monomorphization: https://rustc-dev-guide.rust-lang.org/backend/monomorph.html
- Go Language Specification, Type parameters: https://go.dev/ref/spec
- Go proposal / implementation notes around type parameters and dictionaries
- C++ Templates overview and Itanium ABI name mangling references
