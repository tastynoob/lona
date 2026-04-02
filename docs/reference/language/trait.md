# Trait 与 Trait Dyn

本文描述当前 `lona` 已实现的 trait v0 能力：

- `trait` 顶层声明
- `impl Type: Trait` header
- `Trait.method(&value, ...)` / `Trait.method(ptr, ...)` 静态限定调用
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

## 2. impl header

```lona
struct Point {
    value i32

    def hash() i32 {
        ret self.value + 1
    }
}

impl Point: Hash
```

当前 `impl` 只是 header，不承载方法体。

规则：

- `impl Point: Hash` 表示“现有的 `Point` inherent methods 满足 `Hash`”。
- 编译器会按方法名、receiver access、参数个数、参数 binding kind、参数类型、返回类型检查满足性。
- `impl Type: Trait { ... }` 在 v0 中仍然会被拒绝。
- 同一可见程序图中，`(Trait, Type)` 只能有一份 visible impl。
- orphan rule 仍然生效：`trait` 或 `Type` 至少有一方必须定义在当前模块。

## 3. 静态限定调用

```lona
var point = Point(value = 41)
ret Hash.hash(&point)
```

这条路径是显式静态分派。

规则：

- 必须显式写成 `Trait.method(&value, ...)`，或者在已经有 `Type*` 时写 `Trait.method(ptr, ...)`。
- imported trait 也一样，例如 `dep.Hash.hash(&point)`。
- 第一个源码实参就是显式 receiver；编译器会把它当成 hidden self pointer。
- 这条路径暂时不接受临时值 receiver，例如 `Trait.method(&Point(...), ...)`。
- 编译器会先验证 receiver 的 concrete type 是否有 visible impl。
- 通过后会直接绑定到 concrete inherent method，不经过 witness table。
- getter 需要 `Self const*`；setter 需要 `Self*`。
- 因此 `Trait.bump(&const_value, ...)` 会被拒绝。

当前不做的事：

- trait method 不会自动注入普通 `obj.method(...)` 查找。
- `Hash(point)`、`Hash.hash` 这种把 trait namespace 当 runtime value 的写法没有特殊值语义。

## 4. `Trait dyn`

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

## 5. trait object 构造

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

## 6. dyn receiver 可变性

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

## 7. 常见诊断

当前常见 trait v0 诊断包括：

- trait body 中声明字段、`var`、`global` 或可执行语句
- trait method 在 trait body 中带函数体
- `impl Type: Trait { ... }` body 尚未支持
- `cast[Trait dyn](&temporary)` 的源值不可寻址
- concrete type 没有实现目标 trait
- `Trait.method(value, ...)` 少了显式 self pointer
- `set def` 被调用在只读的 `Trait const dyn` 或 `&const_value` 上

## 8. 实现边界

trait v0 当前不包含：

- 泛型 / trait bound
- default method
- associated type
- auto trait / negative impl
- owning trait object

如果你想看编译器内部怎么做，见 [../../internals/compiler/trait_lowering.md](../../internals/compiler/trait_lowering.md)。
