# 变量定义示例

> 对应 `grammar.md` 的“3.4 变量定义”。
> 本文只讲变量定义和推断。字面量与初始化表达式行为见 [expr.md](expr.md)，类型写法见 [type.md](type.md)。

## 1. 显式类型，不初始化

```lona
var count i32
var flag bool
var point Point
```

## 2. 显式类型，并提供初始化表达式

```lona
var count i32 = 1
var title u8 const[*] = "lona"
var next i32* = null
var ok bool = true
```

## 3. 省略类型，直接由 `var name = expr` 进入后续语义阶段

```lona
var count = 1
var first = 'A'
var name = "compiler"
var point = Point()
```

说明：

- `var name = "compiler"` 当前会推断成 `u8 const[*]`，不是 `str`。
- `var first = 'A'` 当前会推断成 `u8`。
- `var p = null` 不会做类型推断；需要写成 `var p i32* = null` 这类带显式指针类型的形式。
- `var` 绑定不接受最外层 `const` 类型；例如 `var p u8* const = ...`、`var x i32 const = ...` 都会报错。需要只读绑定时改写成 `val p = ...`；如果只是想让 pointee 只读，则写成 `var p u8 const* = ...` 或 `var p u8 const[*] = ...`。
- 字面量转义、字符串底层字节语义和 `null` 的表达式边界见 [expr.md](expr.md)。

## 4. 只读简写 `val name = expr`

```lona
val count = 1
val title = "lona"
val pair = (1, true)
```

说明：

- `val name = expr` 会先按 `var name = expr` 那样推断初始化器的值类型，再在最外层补一层 `const`。
- 因此 `val count = 1` 会得到 `i32 const`，`val pair = (1, true)` 会得到 `<i32, bool> const`。
- 如果推断出来的类型顶层已经是 `const`，这里不会重复叠加第二层同级 `const`。
- 这条语法仍然要求初始化器可推断；例如 `val p = null` 会因为 `null` 缺少具体指针类型而报错。

## 5. 简写形式 `name := expr`

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

## 6. 当前采用值语义与拷贝赋值

说明：

- `var b = a`、`b = a` 这类写法会复制 `a` 当前的值，不会让两个变量自动共享同一份数值、结构体、tuple 或固定维数组存储。
- 指针赋值复制的是地址值，因此两个指针后续仍然可能指向同一份底层对象。
- `ref a T = x` 会把 `a` 绑定成 `x` 的别名；后续 `a = y` 会写入被绑定对象。
- 更完整的指针与 `ref` 语义边界见 [pointer.md](./pointer.md) 与 [ref.md](./ref.md)；结构体字段、方法和 `set` 规则见 [struct.md](./struct.md)。

## 7. 变量声明中也可以直接使用复杂类型

```lona
var p i32*
var grid i32[4, 4]
var handler (i32, i32: i32) = add&<i32, i32>
var allocator (: Ptr) = alloc&<>
var cb (i32: i32) = foo&<i32>
```

这些例子依赖的是 `var_decl ::= IDENT type-name`，其中 `type-name` 可以是指针、数组、函数指针等更复杂的类型写法。
需要注意：

- 指针、数组和函数指针的类型写法统一见 [type.md](type.md)。
- 当前实现额外要求：直接函数指针值变量在定义时必须同时给初始化值；如果是固定数组且元素类型为直接函数指针值，也必须完整初始化，避免缺省补零形成 null 函数指针；外层再包一层 `*` / `[]` / `[*]` 的普通存储类型不受这条限制。
