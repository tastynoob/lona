# 变量定义示例

> 对应 `docs/grammer.md` 的“3.4 变量定义”。

## 1. 显式类型，不初始化

```lona
var count i32
var flag bool
var point Point
```

## 2. 显式类型，并提供初始化表达式

```lona
var count i32 = 1
var title str = "lona"
var ok bool = true
```

## 3. 省略类型，直接由 `var name = expr` 进入后续语义阶段

```lona
var count = 1
var name = "compiler"
var point = Point()
```

## 4. 简写形式 `name := expr`

```lona
count := 1
name := "compiler"
ok := true
```

这一路径在语法里等价于：

```ebnf
IDENT ":" "=" expr
```

因此当前只接受简单标识符，不是成员访问或解引用左值。

## 5. 变量声明中也可以直接使用复杂类型

```lona
var p i32*
var grid i32[4, 4]
var handler (i32, i32) i32
var allocator () Ptr
var cb (i32)* i32 = foo&<i32>
```

这些例子依赖的是 `var_decl ::= IDENT type-name`，其中 `type-name` 可以是指针、数组、函数类型等更复杂的类型写法。
当前实现额外要求：如果变量类型里包着函数类型（例如函数指针、函数相关数组），定义时必须同时给初始化值。
