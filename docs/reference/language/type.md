# 类型写法示例

> 对应 `grammar.md` 的“3.6 类型语法”。
> 本文只讲类型写法本身。字面量、`cast[T](expr)`、结构体类型名的 call-like 初始化和花括号初始化见 [expr.md](expr.md) 与 [vardef.md](vardef.md)；指针语义见 [pointer.md](pointer.md)，`ref` 语义见 [ref.md](ref.md)。

## 1. 内建类型

```lona
var a i32
var b bool
var c f32
var d f64
var e u8 const[*]
```

说明：

- `u8 const[*]` 这个例子展示的是类型后缀的组合方式，也就是 `const` 与 `[*]` 可以继续挂在基础类型后面。
- 内建类型的字面量行为、默认推断和显式转换规则统一见 [expr.md](expr.md)。

## 2. 自定义类型与点分类型

```lona
var p Point
var q math.Point
var r pkg.subpkg.Type
```

## 3. 指针类型

```lona
var p i32*
var q Point*
var r math.Point*
var bytes u8[*]
var rows i8[8]*[*]
```

说明：

- `T*` 表示原始指针，`T[*]` 表示可索引指针。
- `T*` 和 `T[*]` 当前允许隐式转换，但要求元素类型 `T` 完全相同。
- `ref` 不是类型语法，而是绑定 / 参数修饰符；例如 `ref a i32 = x`、`def inc(ref x i32)`。
- 关于 `null`、索引、自动解引用和默认值语义的边界，见 [expr.md](expr.md)、[pointer.md](pointer.md) 和 [ref.md](ref.md)。

## 4. 数组类型

```lona
var row i32[4]
var matrix i32[4][5]
var vector i32[5, 4]
var nested i32[3][4, 5]
```

说明：

- 本节讨论的数组都指“固定维数组”。
- 连续 `[]` 与单个 `[,]` 不是同义写法，而是不同的组合语义。
- `i32[4][5]` 表示语义上的 `5x4` 矩阵，也就是“5 个 `i32[4]`”。
- `i32[5, 4]` 表示语义上的 `5x4` 向量，也就是同层维度组。
- `i32[3][4, 5]` 表示 `4x5` 的 `i32[3]` 向量。
- `i32[]` 这类显式未定长数组类型写法对用户是禁止的，不是“待实现的日常类型语法”。
- 当前编译器会对 `T[]` 给出 targeted diagnostic；旧写法 `T[]*` 也已移除，迁移到 `T[*]`。
- 如果需要稳定语义，请改用固定维数组（如 `i32[4]`）或显式指针（如 `i32*` / `i32[*]` / `i32[4]*`）。
- 数组初始化、索引和维度推断行为见 [expr.md](expr.md) 与 [vardef.md](vardef.md)。

## 5. 元组类型

```lona
var pair <i32, bool>
var triple <i32, bool, f32>
```

说明：

- 元组类型与函数显式取指针 `foo&<...>` 共用 `<...>` 这种类型列表记法。
- 元组字面量、成员访问和类型推断行为见 [expr.md](expr.md)。

## 6. 函数签名与函数指针

```lona
var tick (:) = ping&<>
var cb (i32: i32) = foo&<i32>
var ref_cb (ref i32: i32) = set7&<ref i32>
```

说明：

- 裸函数签名如 `() i32`、`(i32, bool) u8 const[*]` 不是用户可直接存储或传递的一等类型。
- 在类型位置里，parser 只接受显式函数指针写法，例如 `(:)`、`(i32: i32)`、`(ref i32: i32)`。
- 真正有值语义、可赋值、可传参、可间接调用的是函数指针类型。
- 因此 `var f () i32`、`def take(cb () i32)` 这类“裸函数类型”用法在当前实现里会在 parser 阶段直接报错。

其中函数指针类型仍然写在类型层，例如 `(:)` 表示“无参数、无返回值的函数指针”，`(i32: i32)` 表示“参数为 `i32`、返回 `i32` 的函数指针”。
如果函数参数本身按引用传递，则在函数类型里也要显式写出，例如 `(ref i32: i32)`。
与之配套的取值表达式写作 `foo&<i32>` 或 `set7&<ref i32>`；表达式写法见 [expr.md](expr.md)，直接函数指针值变量的初始化约束见 [vardef.md](vardef.md)。

## 7. 函数指针上的数组 / 指针后缀

```lona
var table (: i32)[4]
var slot (i32: i32)*
var cursor (: i32)[*]
```

新的函数指针类型是一个完整的类型原子，所以它后面可以继续接普通的数组 / 指针 / `const` 后缀。

- `(: i32)[4]` 表示“4 个返回 `i32` 的无参函数指针组成的数组”。
- `(i32: i32)*` 表示“指向函数指针值的指针”。
- `(: i32)[*]` 表示“可索引的函数指针视图”。

旧的裸函数派生写法如 `()[] i32`、`(i32)[4] i32`、`()[*] i32` 现在都不是合法类型语法。

## 8. `Trait dyn` 类型

```lona
var h Hash dyn
var imported dep.Hash dyn
```

说明：

- `dyn` 是类型后缀，因此写作 `Hash dyn`、`dep.Hash dyn`。
- 当前 `dyn` 只接受 trait 名，不接受普通结构体类型。
- `Trait dyn` 表示一个显式、非 owning 的 trait object。
- 它不会自动从 `Point` 之类的 concrete value 推导出来；必须显式构造。

当前稳定构造方式是：

```lona
var point = Point(value = 41)
var h Hash dyn = cast[Hash dyn](&point)
```

进一步规则见 [trait.md](trait.md)。
