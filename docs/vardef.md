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
var title u8 const[4] = "lona"
var title_view u8 const[*] = &"lona"
var ok bool = true
```

## 3. 省略类型，直接由 `var name = expr` 进入后续语义阶段

```lona
var count = 1
var name = "compiler"
var point = Point()
```

说明：

- `var name = "compiler"` 当前会推断成 `u8 const[8]`，不是 `str`。
- `var view = &"compiler"` 会推断成 `u8 const[*]`。

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

## 5. 当前采用值语义与拷贝赋值

说明：

- 当前语言还没有单独的“引用类型”；`ref` 是绑定语法，不是 `type-name` 的一部分。
- `var b = a`、`b = a` 这类写法会复制 `a` 当前的值，不会让两个变量自动共享同一份数值、结构体、tuple 或固定维数组存储。
- 指针赋值复制的是地址值，因此两个指针后续仍然可能指向同一份底层对象。
- `ref a T = x` 会把 `a` 绑定成 `x` 的别名；后续 `a = y` 会写入被绑定对象。
- 函数参数和返回值默认按值处理；方法接收者 `self` 则隐式按引用处理。

## 6. 变量声明中也可以直接使用复杂类型

```lona
var p i32*
var grid i32[4, 4]
var handler (i32, i32: i32) = add&<i32, i32>
var allocator (: Ptr) = alloc&<>
var cb (i32: i32) = foo&<i32>
```

这些例子依赖的是 `var_decl ::= IDENT type-name`，其中 `type-name` 可以是指针、数组、函数指针等更复杂的类型写法。
需要注意：

- 裸函数签名如 `() i32`、`(i32, i32) i32` 本身不是有值语义的变量类型。
- parser 只接受显式函数指针写法，因此如果要把函数放进变量里，应该写成 `(:)`、`(i32: i32)` 这类形式，而不是 `() i32`。
- 当前实现额外要求：函数指针变量在定义时必须同时给初始化值。
