# 类型写法示例

> 对应 `docs/grammer.md` 的“3.6 类型语法”。

## 1. 内建类型

```lona
var a i32
var b bool
var c f32
var d f64
var e str
```

说明：

- `f32`、`f64` 当前已经进入词法和类型语法。
- 浮点的完整语义和 lowering 仍在后续里程碑中补齐。
- `str` 目前只保留占位和诊断，不展开运行时表示。

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
```

## 4. 数组类型

```lona
var row i32[4]
var matrix i32[4][5]
var vector i32[5, 4]
var nested i32[3][4, 5]
```

说明：

- 连续 `[]` 与单个 `[,]` 不是同义写法，而是不同的组合语义。
- `i32[4][5]` 表示语义上的 `5x4` 矩阵，也就是“5 个 `i32[4]`”。
- `i32[5, 4]` 表示语义上的 `5x4` 向量，也就是同层维度组。
- `i32[3][4, 5]` 表示 `4x5` 的 `i32[3]` 向量。
- 固定维度数组的完整语义和 lowering 仍在后续里程碑中补齐；当前这一轮主要先收口语法和 AST。

## 5. 元组类型

```lona
var pair <i32, bool>
var triple <i32, bool, f32>
```

说明：

- 元组类型与函数显式取指针 `foo&<...>` 共用 `<...>` 这种类型列表记法。
- 当前 milestone 只把元组类型接进了 parser / AST；完整的 tuple 语义和 lowering 仍是后续工作。

## 6. 函数类型

```lona
var f () i32
var g (i32) i32
var h (i32, bool) str
var cb (i32)* i32 = foo&<i32>
```

函数类型在当前 grammar 里写成“参数头 + 返回类型”，也就是：

```ebnf
type-name ::= func-head type-name
```

其中函数指针类型仍然写在类型层，例如 `(i32)* i32` 表示“参数为 `i32`、返回 `i32` 的函数指针”。
与之配套的取值表达式写作 `foo&<i32>`，表示显式取得 `def foo(v i32)` 这个函数的指针。
当前实现里，这类“包含函数类型”的变量存储若不是裸函数本体，也要求在定义时同时给出初始化值，避免出现未初始化的函数指针/函数相关数组。

## 7. 函数头上的数组后缀

```lona
var table ()[] i32
var matrix (i32)[4] i32
```

这类形式来自 `func-head` 继续接 `[]` 或 `[expr-seq]` 的规则。它们是当前文法允许的形态，但文档这里只说明语法外形，不展开更深的类型语义解释。
如果把这类函数相关类型用于变量存储，当前实现还要求变量定义时就完成初始化。
