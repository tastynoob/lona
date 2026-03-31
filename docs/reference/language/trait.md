# Trait 与 Trait Dyn

本文描述当前 `lona` 已实现的 trait v0 能力：

- `trait` 顶层声明
- `impl Type: Trait` header
- `Trait.method(value, ...)` 静态限定调用
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
ret Hash.hash(point)
```

这条路径是显式静态分派。

规则：

- 必须显式写成 `Trait.method(value, ...)`。
- imported trait 也一样，例如 `dep.Hash.hash(point)`。
- 编译器会先验证 receiver 的 concrete type 是否有 visible impl。
- 通过后会直接绑定到 concrete inherent method，不经过 witness table。

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

## 6. dyn compatibility

不是所有 trait 都能构造 `Trait dyn`。

当前 v0 约束是：

- `Trait dyn` 只支持 get-only methods。
- 只要 trait 中出现 `set def`，这个 trait 就不能用于 `Trait dyn` 构造或动态调用。
- 这种 trait 仍然可以继续走静态 `Trait.method(value, ...)` 路径。

例如：

```lona
trait CounterLike {
    set def bump(step i32) i32
}
```

`CounterLike dyn` 当前会被拒绝。

## 7. 常见诊断

当前常见 trait v0 诊断包括：

- trait body 中声明字段、`var`、`global` 或可执行语句
- trait method 在 trait body 中带函数体
- `impl Type: Trait { ... }` body 尚未支持
- `cast[Trait dyn](&temporary)` 的源值不可寻址
- concrete type 没有实现目标 trait
- 目标 trait 不是 dyn-compatible

## 8. 实现边界

trait v0 当前不包含：

- 泛型 / trait bound
- default method
- associated type
- auto trait / negative impl
- owning trait object

如果你想看编译器内部怎么做，见 [../../internals/compiler/trait_lowering.md](../../internals/compiler/trait_lowering.md)。
