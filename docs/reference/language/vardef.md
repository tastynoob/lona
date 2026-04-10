# 变量定义示例

> 对应 `grammar.md` 的“3.4 变量定义”。
> 本文只讲变量定义和推断。字面量与初始化表达式行为见 [expr.md](expr.md)，类型写法见 [type.md](type.md)。
> 前缀 `const`、最外层 `const`、按值物化和写入判定的统一规则见 [mutability.md](mutability.md)。

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
- `var` 绑定不接受最外层 `const` 类型；例如 `var p u8* const = ...`、`var x i32 const = ...` 都会报错。需要只读绑定时改写成 `const p = ...` 或 `const p T = ...`；如果只是想让 pointee 只读，则写成 `var p u8 const* = ...` 或 `var p u8 const[*] = ...`。
- 字面量转义、字符串底层字节语义和 `null` 的表达式边界见 [expr.md](expr.md)。

## 4. 只读绑定 `const name = expr` / `const name T = expr`

```lona
const count = 1
const title = "lona"
const pair = (1, true)
const view u8 const[*] = "lona"
```

说明：

- `const name = expr` 会先按 `var name = expr` 那样推断初始化器的值类型，再在最外层补一层 `const`。
- `const name T = expr` 会先按 `T` 做初始化检查，再把绑定结果视为最外层 `const`。
- 因此 `const count = 1` 会得到 `i32 const`，`const pair = (1, true)` 会得到 `<i32, bool> const`，`const view u8 const[*] = "lona"` 会得到 `u8 const[*] const`。
- 如果推断出来的类型顶层已经是 `const`，这里不会重复叠加第二层同级 `const`。
- 这条语法仍然要求初始化器可推断；例如 `const p = null` 会因为 `null` 缺少具体指针类型而报错。

## 5. 编译期常量绑定 `inline name = expr` / `inline name T = expr`

```lona
inline size = 4
inline ratio f64 = 0.25
inline msg = "lona"

def local() i32 {
    inline twice = size * 2
    ret twice
}
```

说明：

- `inline` 表示“编译期常量绑定”，不是文本替换，也不是函数内联。
- `inline name = expr` 只有在类型推断和值求值都能在编译期完成时才合法；例如 `inline p = null` 仍然会因为缺少目标指针类型而报错。
- 当前 `inline` 绑定值只支持内建标量和指针类型，例如 `i32`、`f64`、`bool`、`T*`、`T[*]`。
- 初始化器必须属于当前支持的编译期常量表达式子集：标量字面量、字符串字面量、`null`、已有 `inline` 绑定、支持的内建一元/二元运算、`cast[T](expr)`、`sizeof`。
- `inline` 不能依赖运行时值；例如 `var x = 1; inline y = x` 会报错。
- `inline` 绑定不分配运行时存储槽位，因此不能取 `&`，也不能作为 `ref` 实参传递。

### 5.1 顶层 `inline`

```lona
// dep.lo
inline answer = 42

// main.lo
import dep

inline local = dep.answer + 1

def read_local() i32 {
    ret local
}

def read_dep() i32 {
    ret dep.answer
}
```

说明：

- `inline` 既可以出现在块内，也可以出现在文件顶层。
- 顶层 `inline` 在同模块里按普通名字可见；后续函数和顶层语句可以直接读取它。
- importer 可以通过 `file.xxx` 访问被导入模块的顶层 `inline` 常量，例如 `dep.answer`。
- 顶层 `inline` 仍然不是 `global`；它会进入模块接口，但不会物化成独立的运行时全局符号。

## 6. 简写形式 `name := expr`

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

## 7. 当前采用值语义与拷贝赋值

说明：

- `var b = a`、`b = a` 这类写法会复制 `a` 当前的值，不会让两个变量自动共享同一份数值、结构体、tuple 或固定维数组存储。
- 指针赋值复制的是地址值，因此两个指针后续仍然可能指向同一份底层对象。
- `ref a T = x` 会把 `a` 绑定成 `x` 的别名；后续 `a = y` 会写入被绑定对象。
- 更完整的指针与 `ref` 语义边界见 [pointer.md](./pointer.md) 与 [ref.md](./ref.md)；结构体字段、方法和 `set` 规则见 [struct.md](./struct.md)。

## 8. 变量声明中也可以直接使用复杂类型

```lona
var p i32*
var grid i32[4, 4]
var handler (i32, i32: i32) = @add
var allocator (: Ptr) = @alloc
var cb (i32: i32) = @foo
```

这些例子依赖的是 `var_decl ::= IDENT type-name`，其中 `type-name` 可以是指针、数组、函数指针等更复杂的类型写法。
需要注意：

- 指针、数组和函数指针的类型写法统一见 [type.md](type.md)。
- 当前实现额外要求：直接函数指针值变量在定义时必须同时给初始化值；如果是固定数组且元素类型为直接函数指针值，也必须完整初始化，避免缺省补零形成 null 函数指针；外层再包一层 `*` / `[]` / `[*]` 的普通存储类型不受这条限制。
