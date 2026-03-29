# 绑定、`const` 与可写性

> 本文收口当前 `lona` 里 `var` / 前缀 `const` / `ref` / `set` 的统一规则。
> 变量定义细节见 [vardef.md](vardef.md)，`ref` 绑定与参数见 [ref.md](ref.md)，结构体字段与方法见 [struct.md](struct.md)，指针见 [pointer.md](pointer.md)。

## 1. 这不是四套独立机制

当前可以把这部分语义拆成“三条主轴 + 一个表面复用”：

- `var` / `ref`：名字如何绑定
- `T const`：当前类型视图是否可写
- `set`：成员投影和方法接收者是否给出可写视图
- `const name ... = expr`：只是把“只读绑定”直接写成 `const`

更具体地说：

- `var x T = expr`：为 `x` 建立一个普通值绑定
- `ref x T = expr`：把 `x` 绑定成已有存储位置的别名
- `T const`：冻结当前这个类型节点
- `set field` / `set def`：控制结构体外部字段投影和方法 receiver 的可写性
- `const x = expr`：先按值推断 `expr`，再把结果变成最外层 `const`
- `const x T = expr`：先按 `T` 做初始化检查，再把绑定结果变成最外层 `const`

也就是说，`const` 在表面语法里同时做两件事：

- 作为 prefix binding，引入只读绑定
- 作为 postfix qualifier，修饰类型节点

但底层仍然只有一套 `const` 语义，不是两种互不相干的机制。

## 2. `var` 与前缀 `const`

### 2.1 `var`

```lona
var count = 1
var p i32* = null
```

`var` 本身不表示“可写视图”或“只读视图”，它只表示这里建立的是一个普通值绑定。

如果写了显式类型，就直接使用这个存储类型。
如果省略类型，就按初始化表达式的**按值物化类型**推断。

需要注意：

- `const` 仍然是正常的类型系统能力，不只存在于只读绑定
- 但当前显式 `var x T const = ...` 这种“顶层 `const` 存储”会被拒绝
- 如果只是想做只读绑定，用 `const`
- 如果只是想让 pointee / element 只读，就把 `const` 放到更深层，例如 `T const*`、`T const[*]`

### 2.2 前缀 `const`

```lona
const count = 1                 // i32 const
const pair = (1, true)          // <i32, bool> const
const view u8 const[*] = "lona" // u8 const[*] const
```

前缀 `const` 不是另一套绑定模型。
它只是：

1. 先像普通变量那样做推断或类型检查
2. 再在最外层补一层 `const`

因此：

- `const x = 1` 得到 `i32 const`
- `const p = &x` 得到 `i32* const`
- `const s = "hi"` 得到 `u8 const[*] const`
- `const p u8 const[*] = "hi"` 得到 `u8 const[*] const`

如果结果顶层已经是 `const`，这里不会再重复叠加第二层同级 `const`。

需要注意，`const p = &x` 只冻结这个**指针槽位**，不自动冻结 pointee。
也就是说：

- `p = other` 不允许
- `*p = value` 是否允许，取决于 pointee 类型本身是否可写

## 3. `const` 的当前语义

### 3.1 `const` 是 postfix type qualifier

`const` 总是修饰左边刚形成的类型节点。

```lona
u8 const[*]
u8[*] const
u8 const* const
```

它们分别表示：

- 指向 `const u8` 的可索引指针
- `const` 的可索引指针
- `const` 的原始指针，指向 `const u8`

### 3.2 `const` 默认是 shallow 的

`T const` 只冻结当前这一层，不递归冻结更深层对象。

因此：

- `u8 const[*]` 与 `u8[*] const` 是不同类型
- `u8 const[4]` 与 `u8[4] const` 是不同类型

### 3.3 按值物化会丢掉“被复制出来的那一层 `const`”

当一个表达式被按值复制成新对象时，被复制出来的那一层 `const` 可以丢掉。

这条规则只影响真正发生值复制的层级：

- 标量复制：新值可以去掉这一层 `const`
- 数组 / tuple 复制：外层和值成员一起物化，所以成员层的 `const` 也会一起重新物化
- 指针复制：只复制地址值，不复制 pointee，所以 pointee 的 `const` 不会被去掉

例如：

```lona
var a u8 const[2] = {1, 2}
var b = a            // b: u8[2]

const p = &b(0)      // p: u8* const
var q = p            // q: u8*
```

但下面这类情况不会去掉更深层 `const`：

```lona
var s = "hi"       // s: u8 const[*]
```

这里复制出来的是“指针视图”本身，不是底层 bytes，所以元素仍然是 `const u8`。

## 4. 写入的统一判定

当前所有写入都可以按同一条规则理解：

1. 左侧必须是可寻址位置
2. 左侧当前投影出来的类型必须是 fully writable

可寻址左值包括：

- 变量
- 结构体字段
- 固定数组元素
- `*ptr`

只要当前投影类型里包含只读存储，这个位置就不能写。

例如：

```lona
const value = 1
value = 2          // 不允许

var bytes u8 const[2] = {1, 2}
bytes(0) = 7       // 不允许

const view = cast[i32[*]](&data(0))
view(0) = 7        // 允许，冻结的是指针槽位，不是元素
```

## 5. `ref` 只表示别名

`ref` 不是类型语法。
它只表示：

- 局部别名绑定
- `ref` 参数

默认赋值、传参与返回仍然按值处理。

### 5.1 `ref` 绑定要求可寻址

```lona
ref a i32 = x
ref b i32 = arr(0)
ref c i32 = point.x
ref d i32 = *ptr
```

右侧必须是稳定存储位置，不能是纯右值。

### 5.2 `ref` 不等于 mutable

`ref` 只表示“别名”，不表示“这个视图一定可写”。

下面这种写法是合法的：

```lona
var x i32 = 1
ref a i32 const = x
```

这里 `a` 是 `x` 的别名，但它看到的是 `i32 const` 视图，因此不能通过 `a` 写入。

### 5.3 `ref` 只能增加 `const`，不能丢掉 `const`

调用 `ref` 参数或建立局部 `ref` 绑定时，允许把可写对象绑定成只读视图：

```lona
def read(ref x i32 const) i32 {
    ret x
}

var x i32 = 3
read(ref x)        // 允许
```

但不允许把已有只读存储重新绑定成更可写的视图：

```lona
def bump(ref x i32) i32 {
    x = x + 1
    ret x
}

const x = 3
bump(ref x)        // 不允许
```

## 6. `set` 的统一含义

`set` 不是单独的权限系统。
它只决定结构体相关投影是否给出 writable view。

### 6.1 `set field`

```lona
struct Counter {
    value i32
    set current i32
}
```

对结构体外部来说：

- `obj.value` 投影成 `i32 const`
- `obj.current` 投影成 `i32`

对指针字段，未标 `set` 只冻结字段槽位本身，不自动冻结 pointee：

```lona
struct Box {
    ptr i32*
}
```

这里外部不能写 `obj.ptr = other`，但仍然可以写 `*obj.ptr = 7`。

### 6.2 `set def`

方法的 hidden receiver 总是指针，但 receiver pointee 是否带 `const` 由 `set` 决定：

- 普通 `def`：`self Self const*`
- `set def`：`self Self*`

因此：

- 普通 `def` 里不能改写 `self`
- `set def` 才能通过当前 receiver 改写对象

### 6.3 字段 `set` 不约束结构体内部的 writable receiver

在同一个结构体的方法体里，字段是不是 `set field` 不决定能不能内部写入；
真正决定因素是当前 receiver 是 `Self const*` 还是 `Self*`。

也就是说：

- 普通 `def`：即使字段写成 `set value`，也不能写 `self.value = ...`
- `set def`：即使字段本身不是 `set field`，也可以通过当前 writable receiver 改写自身槽位

## 7. 结构体字段自身必须是 fully writable storage type

当前结构体字段还有一条额外约束：

- 字段类型本身必须能作为完整可写存储存在

因此下面这类字段会被拒绝：

```lona
struct Bad {
    value u8 const
    bytes u8 const[4]
    pair <i32 const, i32>
}
```

原因不是这些类型“永远不合法”，而是当前字段模型要求字段对象本身在初始化期保持完整可写。

但“指向 const 的指针视图”仍然允许做字段：

```lona
struct Span {
    data u8 const[*]
}
```

因为这里冻结的是 pointee 视图，不是字段槽位本身。

## 8. 一页总结

- `var` / `ref` 管绑定
- `const` 管当前类型视图是否可写，前缀 `const` 绑定只是这套语义的表面写法
- `set` 管结构体成员投影和 receiver 是否给出 writable view
- 按值复制会丢掉“被复制出来的那一层 `const`”
- 指针复制不会去掉 pointee 的 `const`
- `ref` 只建立别名；它可以增加 `const`，不能丢掉已有 `const`
- 所有写入都要求“左侧可寻址，且当前投影类型 fully writable”
