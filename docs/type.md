# 类型写法示例

> 对应 `docs/grammer.md` 的“3.6 类型语法”。

## 1. 内建类型

```lona
var a i32
var b bool
var c str
```

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
var data i32[]
var row i32[4]
var grid i32[4, 4]
var dyn i32[, 4]
```

这里分别对应：

- `T[]`
- `T[n]`
- `T[n1, n2, ...]`
- `T[,n1, n2, ...]`

## 5. 指针与数组组合

```lona
var a i32*[]
var b i32[]*
var c Point*[2]
```

这些例子只说明当前文法允许这种组合写法，具体类型语义由后续阶段决定。

## 6. 函数类型

```lona
var f () i32
var g (i32) i32
var h (i32, bool) str
```

函数类型在当前 grammar 里写成“参数头 + 返回类型”，也就是：

```ebnf
type-name ::= func-head type-name
```

## 7. 函数头上的数组后缀

```lona
var table ()[] i32
var matrix (i32)[4] i32
```

这类形式来自 `func_head` 继续接 `[]` 或 `[expr-seq]` 的规则。它们是当前文法允许的形态，但文档这里只说明语法外形，不展开更深的类型语义解释。
