# Trait 与 Trait Dyn

本文描述当前 `lona` 已实现的 trait v0 能力：

- `trait` 顶层声明
- `impl Trait for Type { ... }` 这类带方法体的 impl
- `impl[T Trait] Trait for Box[T]` 这类单 bound generic impl
- `def func[T Trait](value T)` 这类单 trait bound generic function
- `Trait.method(&value, ...)` / `Trait.method(ptr, ...)` 静态限定调用
- `value.Trait.method(...)` / `ptr.Trait.method(...)` 显式 receiver trait 路径
- `Trait dyn` 显式、非 owning 的动态 trait object
- `cast[Trait dyn](&value)` 借用式构造

本文只描述用户可见的语法和语义规则。
如果你关心 witness table、slot 顺序和 LLVM lowering，见
[../../internals/compiler/trait_lowering.md](../../internals/compiler/trait_lowering.md)。

## 1. trait 声明

```lona
trait Hash {
    def hash() i32
}
```

当前规则：

- `trait` 是新的顶层声明。
- trait body 里只允许方法签名。
- trait 方法可以写 `set def`，receiver access 会参与 impl 满足性检查。
- trait 方法当前不能在 trait body 里带函数体。

例如：

```lona
trait CounterLike {
    set def bump(step i32) i32
}
```

## 2. impl

```lona
struct Point {
    value i32
}

impl Hash for Point {
    def hash() i32 {
        ret self.value + 1
    }
}
```

规则：

- `impl Hash for Point { ... }` 是当前唯一的 trait 实现语法。
- `impl Hash for Point { ... }` 允许直接在 impl body 里写 trait 方法实现。
- 这类 impl body 方法属于 trait 专属方法命名空间，不会和普通 inherent method 共用同一个方法槽。
- `impl[T Trait] Trait for Box[T] { ... }` 表示“对所有满足该单 bound 的具体实例，都提供一份显式 trait 实现”。
- 编译器会按方法名、receiver access、参数个数、参数 binding kind、参数类型、返回类型检查 impl body 与 trait 声明是否一致。
- `impl Trait for Type { ... }` 已支持 local self、imported self、applied self 和 generic self。
- 这版 impl body 里只允许 trait 已声明的方法定义；不允许额外 helper method。
- trait 已声明的方法必须全部在 impl body 里显式给出。
- struct 声明现在支持单 bound，例如 `struct Box[T Hash]`。
- struct 方法现在也可以带自己的 generic parameter，例如
  `def map[U](...)` 或 `def merge[U Hash](...)`。
- trait 方法本身仍然不能声明 generic parameter。
- 同一可见程序图中，`(Trait, Type)` 只能有一份 visible impl。
- orphan rule 仍然生效：`trait` 或 `Type` 至少有一方必须定义在当前模块。

当前 first cut 边界：

- `Trait.method(&value, ...)`、`value.Trait.method(...)` 和 `Trait dyn` 都会绑定到同一份 trait 实现；`obj.method()` 在选择到该 trait 方法时也会命中同一份实现。
- local concrete type 上，trait impl body 方法可以和 inherent method 同名，也可以和其他 trait 的同名方法并存。
- impl body 中的方法仍然必须逐项对应 trait 已声明的方法签名。

## 3. 静态限定调用

```lona
var point = Point(value = 41)
ret Hash.hash(&point)
```

这条路径是显式静态分派。

规则：

- 必须显式写成 `Trait.method(&value, ...)`，或者在已经有 `Type*` 时写 `Trait.method(ptr, ...)`。
- imported trait 也一样，例如 `dep.Hash.hash(&point)`。
- bounded generic body 里这条路径仍然可用；例如 `def hash_one[T Hash](value T) i32 { ret Hash.hash(&value) }`。
- 第一个源码实参就是显式 receiver；编译器会把它当成 hidden self pointer。
- 这条路径暂时不接受临时值 receiver，例如 `Trait.method(&Point(...), ...)`。
- 编译器会先验证 receiver 的 concrete type 是否有 visible impl。
- 通过后会直接绑定到 concrete method 实现；如果方法来自 `impl Trait for Type { ... }`，也会绑定到这份实现，不经过 witness table。
- getter 需要 `Self const*`；setter 需要 `Self*`。
- 因此 `Trait.bump(&const_value, ...)` 会被拒绝。

当前不做的事：

- `Hash(point)`、`Hash.hash` 这种把 trait namespace 当 runtime value 的写法没有特殊值语义。
- unconstrained `T` 仍然不会自动得到普通 `obj.method(...)` 查找；没有 bound 时继续写 `Hash.hash(&value)` 也不成立，因为编译器还不知道 `T` 满足哪个 trait。

## 4. 显式 receiver trait 路径

除了静态限定调用，还可以直接在 receiver 后面写 trait 名：

```lona
var point = Point(value = 41)
ret point.Hash.hash()
```

规则：

- 当前稳定形式是 `value.Trait.method(...)` 和 `ptr.Trait.method(...)`。
- 这里的 `Trait` 必须是当前模块里定义的 trait 名。
- receiver 必须是 concrete struct value，或者可解引用到 concrete struct 的 `Type*`。
- 这条路径会先验证该 concrete type 是否存在 visible impl。
- getter / setter 的 receiver 可写性规则和普通 trait 调用一致；`set def` 仍然要求可写 receiver。
- 如果对象上真的存在名为 `Trait` 的普通成员路径，那么普通成员路径优先，不会强行改按 trait 路径解释。

例如：

```lona
trait Hash {
    def read() i32
}

trait Metric {
    def read() i32
}

struct Point {
    value i32
}

impl Hash for Point {
    def read() i32 {
        ret self.value + 1
    }
}

impl Metric for Point {
    def read() i32 {
        ret self.value + 2
    }
}

var point = Point(value = 40)
ret point.Hash.read() + point.Metric.read()
```

当前边界：

- imported trait 还不支持 `value.dep.Trait.method(...)` 这种多段 receiver path；这类场景继续写 `dep.Trait.method(&value, ...)`。
- bounded generic `T` 目前不走 `value.Trait.method(...)` 这条路径；generic body 里继续使用 `value.method()` 或 `Trait.method(&value, ...)`。

## 5. `Trait dyn`

```lona
var point = Point(value = 41)
var h Hash dyn = cast[Hash dyn](&point)
ret h.hash()
```

`Trait dyn` 是显式动态分派路径。

规则：

- `dyn` 是类型后缀，因此写作 `Hash dyn`、`dep.Hash dyn`。
- `Trait dyn` 是非 owning 的借用型 trait object。
- 它不会改变原始对象布局；`Point(value = 41)` 仍然只是 `Point`。
- 只有显式写出 `cast[Hash dyn](...)` 时，才会构造 trait object。
- 如果借用源是只读的，构造结果会自动变成只读 trait object，也就是 `Hash const dyn`。

## 6. trait object 构造

当前稳定构造语法是：

```lona
var h Hash dyn = cast[Hash dyn](&point)
```

要求：

- 必须是显式 `cast[...]`。
- 源表达式必须写成显式借用 `&value`。
- 被借用的值必须可寻址，例如变量、字段、解引用指针或数组元素。
- source type 必须有 visible impl。
- 如果 `&value` 的 pointee 是 `const`，结果会自动保留为 `Trait const dyn`；不要求额外写 `cast[Hash const dyn](...)`。

例如 imported trait 也可以这样写：

```lona
import dep

var point dep.Point = dep.make()
var h dep.Hash dyn = cast[dep.Hash dyn](&point)
ret h.hash()
```

当前明确不支持：

- `cast[Hash dyn](Point(...))`
- 从临时值直接构造 trait object
- owning trait object

## 7. dyn receiver 可变性

`Trait dyn` 现在不再按“整个 trait 是否 dyn-compatible”一刀切，而是按“当前方法 + 当前 receiver”的组合检查。

规则：

- getter-style 方法可以在 `Trait dyn` 和 `Trait const dyn` 上调用。
- `set def` 只可以在可写的 `Trait dyn` 上调用。
- `cast[Trait dyn](&value)` 只表示“构造 trait object”；结果是可写还是只读，由借用源的 pointee constness 决定。
- 因此 mixed getter/setter trait 也可以构造 dyn object。

例如：

```lona
trait CounterLike {
    def read() i32
    set def bump(step i32) i32
}
```

此时：

- `cast[CounterLike dyn](&counter)` 可以调用 `read()` 和 `bump(...)`
- `cast[CounterLike dyn](&const_counter)` 会得到 `CounterLike const dyn`
- `CounterLike const dyn` 只能调用 `read()`，不能调用 `bump(...)`

## 8. 常见诊断

当前常见 trait v0 诊断包括：

- trait body 中声明字段、`var`、`global` 或可执行语句
- trait method 在 trait body 中带函数体
- impl body 中定义了 trait 未声明的方法，或方法签名与 trait 声明不一致
- `obj.method()` 命中多个同名 trait 方法时的歧义错误
- `cast[Trait dyn](&temporary)` 的源值不可寻址
- concrete type 没有实现目标 trait
- `Trait.method(value, ...)` 少了显式 self pointer
- `set def` 被调用在只读的 `Trait const dyn`、`&const_value` 或只读的 `value.Trait.method(...)` receiver 上

## 9. 方法命名与歧义

trait v0 现在已经把 trait 方法和 ordinary inherent method 分到不同命名空间。

当前实现模型是：

- `impl Trait for Type { ... }` 会给 `(SelfType, Trait, Method)` 注册一份 trait 专属实现。
- `Trait.method(&value, ...)`、`value.Trait.method(...)` 和 `Trait dyn` 都优先绑定到这份 trait 专属实现。
- ordinary inherent method 仍然留在普通方法命名空间里。

普通 `obj.method(...)` 的查找优先级是：

1. 先找 ordinary inherent method。
2. 如果没有 inherent method，再看同名 trait 方法。
3. 如果同名 trait 方法只有一个，则允许直接调用。
4. 如果同名 trait 方法有多个，则报歧义，要求显式写 trait 身份。

例如：

```lona
trait Hash {
    def read() i32
}

trait Metric {
    def read() i32
}

struct Point {
    value i32

    def read() i32 {
        ret self.value + 100
    }
}

impl Hash for Point {
    def read() i32 {
        ret self.value + 1
    }
}

impl Metric for Point {
    def read() i32 {
        ret self.value + 2
    }
}
```

此时：

- `point.read()` 调用 inherent method。
- `point.Hash.read()` 调用 `Hash` 的 trait 方法实现。
- `point.Metric.read()` 调用 `Metric` 的 trait 方法实现。
- `Hash.read(&point)` 和 `Metric.read(&point)` 也分别命中对应 trait 实现。

如果去掉 inherent method，那么：

- 当只有一个同名 trait 方法时，`point.read()` 仍然允许。
- 当有多个同名 trait 方法时，`point.read()` 会报歧义。

当前边界：

- imported trait 同名歧义目前建议用静态限定调用来消歧，例如 `left.Hash.read(&point)`、`right.Hash.read(&point)`。
- `value.Trait.method(...)` 只支持单段 trait 名，不支持 `value.dep.Trait.method(...)`。
- 由于语言当前不支持重载，trait 身份也不会把“同名但不同参数签名”的普通方法调用变成重载分派系统。

## 10. 泛型与 single bound

trait v0 现在已经支持最小闭合的 generic + trait 组合：

```lona
trait Hash {
    def hash() i32
}

struct Point {
    value i32

}

impl Hash for Point {
    def hash() i32 {
        ret self.value + 1
    }
}

def hash_one[T Hash](value T) i32 {
    ret value.hash()
}

struct Box[T Hash] {
    value T

    def echo[U](value U) U {
        ret value
    }
}

impl[T Hash] Hash for Box[T] {
    def hash() i32 {
        ret Hash.hash(&self.value)
    }
}
```

当前规则：

- 每个 type parameter 只支持一个 trait bound，例如 `T Hash`。
- bound satisfaction 在实例化点检查，而不是模板声明点提前假设成立。
- same-module 和 imported generic instantiation 都会检查 bound。
- struct 声明位置也支持同样的 single bound，例如 `struct Box[T Hash]`。
- `impl[T Trait] Trait for Box[T]` 的 self type 使用普通的 `Box[T]` 声明语法，不走任何额外的类型字符串特例。
- generic struct method 允许声明自己的 type parameter；实例化同样支持 same-module 和 imported 调用。
- generic struct method 的 bound 也按实例化点检查。
- generic function / method body 中，bounded `T` 允许直接调用 bound trait method，例如 `value.hash()`。
- 显式 trait-qualified static call `Hash.hash(&value)` 仍然可用，特别适合需要强调 trait 身份或避免命名歧义时。
- bounded `T` 仍然不开放裸字段访问、字段写入与运算符；bound 只会放开“直接方法调用”这一种 dot 形态。
- `value.Trait.method(...)` 这条 receiver trait path 当前仍然要求 concrete struct receiver，因此不作为 bounded generic 的稳定语法。

当前明确不支持：

- multi-bound，例如 `[T Hash + Eq]`
- trait method 自己再带 generic parameter
- default method、associated type、generic trait、negative impl

如果你想先看 generic v0 的整体入口，包括 `Type[...]`、`name[T](...)`、`@name[T]`、推断和 unconstrained/bounded 参数边界，见 [generic.md](generic.md)。

## 11. 实现边界

trait v0 当前不包含：

- default method
- associated type
- auto trait / negative impl
- owning trait object
- multi-bound generic constraints
- generic trait

如果你想看编译器内部怎么做，见 [../../internals/compiler/trait_lowering.md](../../internals/compiler/trait_lowering.md)。
