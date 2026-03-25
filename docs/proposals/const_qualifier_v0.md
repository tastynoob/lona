# Const Qualifier v0

> 这是一份 `const` 类型修饰符的第一版草案。
> 目标是给 `lona` 引入稳定、可组合的“不可改写”类型语义，同时保持它与现有 `var` / `ref`、数组、指针和字节串设计分层清晰。
>
> 注意：文中涉及字符串的例子保留的是早期数组化字符串模型。当前实现里的字符串字面量已经收口为 UTF-8 `u8 const[*]` 视图，并自动追加结尾 `0` 字节。

## 1. 状态与范围

`const_qualifier_v0` 只定义：

- `const` 是类型修饰，而不是定义修饰
- `const` 的结合方式
- `const` 在数组、指针、可索引指针上的 shallow 语义
- 按值物化时如何丢弃当层 `const`
- 与字符串字节流的关系

这份草案**不**定义：

- 全局常量求值
- 编译期常量表达式系统
- `constexpr` / `static readonly` 一类扩展
- 只读内存段的完整后端实现细节

因此，这里讨论的是：

- “某个类型视图能不能改”

而不是：

- “这个值是不是编译期已知”

## 2. 基本原则

### 2.1 `const` 是类型修饰

`const` 不属于变量定义修饰。

也就是说：

- `var`
- `ref`

属于“定义 / 绑定修饰”。

而：

- `const`

属于“类型修饰”。

例如：

```lona
var p u8[*] const
ref p u8 const[*]
```

这里：

- `var` / `ref` 决定这个名字如何绑定
- `const` 决定这个类型视图是否可写

### 2.2 `const` 总是修饰左边刚形成的类型节点

`const` 是 postfix type qualifier。

阅读规则是：

- 从左到右构造类型
- `const` 总是给它左边刚刚形成的那个类型节点加限定

例如：

```lona
u8[*] const
u8 const[*]
u8 const* const
```

分别表示：

- 常量的可索引指针
- 指向常量 `u8` 的可索引指针
- 常量指针，指向常量 `u8`

这条规则适用于：

- 基础类型
- 数组
- 原始指针 `*`
- 可索引指针 `[*]`

### 2.3 `const` 是 shallow qualifier

`const` 默认是 shallow 的，不递归传播到内部对象。

也就是说：

- `T const` 只冻结当前这个 `T`
- 不自动把它的成员、元素或 pointee 也递归变成 `const`

因此：

- `u8 const[4]` 和 `u8[4] const` 是不同类型
- `u8 const[*]` 和 `u8[*] const` 也是不同类型

## 3. 数组语义

### 3.1 `u8 const[4]`

```lona
var arr u8 const[4]
```

这里的含义是：

- 数组元素类型是 `u8 const`

因此：

- `arr(0) = 1`：报错
- `arr = other`：允许

但读取元素如果按值物化成新对象，则可以丢掉这一层的 `const`：

```lona
var a = arr(0)      // a: u8
var b u8 = arr(0)   // 显式复制成可变 u8
```

### 3.2 `u8[4] const`

```lona
var arr u8[4] const
```

这里的含义是：

- 数组值本身是 `const`
- 元素类型仍然是普通 `u8`

因此：

- `arr = other`：报错
- `arr(0) = 1`：允许

读取元素时，不会凭空得到 `const` 元素：

```lona
var a = arr(0)   // a: u8
```

### 3.3 这两种类型都合法且都保留

`const_qualifier_v0` 不把这两种写法 canonicalize 成同一个语义。

原因是它们表达的意图不同：

- `u8 const[4]`：元素不可改
- `u8[4] const`：整体不可整体赋值替换

## 4. 指针语义

### 4.1 原始指针

```lona
u8 const*
u8* const
u8 const* const
```

分别表示：

- 指向常量 `u8` 的原始指针
- 常量原始指针
- 常量原始指针，指向常量 `u8`

对应语义：

- `u8 const*`
  - `*p = 1`：报错
  - `p = other`：允许
- `u8* const`
  - `*p = 1`：允许
  - `p = other`：报错
- `u8 const* const`
  - `*p = 1`：报错
  - `p = other`：报错

### 4.2 可索引指针

同样的规则也适用于 `[*]`：

```lona
u8 const[*]
u8[*] const
u8 const[*] const
```

分别表示：

- 指向常量 `u8` 的可索引指针
- 常量可索引指针
- 常量可索引指针，指向常量 `u8`

对应语义：

- `u8 const[*]`
  - `p(0) = 1`：报错
  - `p = other`：允许
- `u8[*] const`
  - `p(0) = 1`：允许
  - `p = other`：报错
- `u8 const[*] const`
  - `p(0) = 1`：报错
  - `p = other`：报错

## 5. 自动推断与显式类型

### 5.1 `var` 推断的是“按值物化后的类型”

`const` 是类型的一部分，但：

- `var x = expr`

不直接照抄 `expr` 的原始视图类型，而是推断：

- 把 `expr` 按值物化成一个新对象之后的类型

核心规则可以总结为一句话：

- 当层值拷贝允许丢弃当层的 `const` 描述

也就是说：

- 哪一层对象真的被复制出了一个新值，就允许在那一层丢掉 `const`
- 没有被复制、只是继续借用/指向的更深层对象，仍然保留原有 `const`

这比“永远精确保留 `const`”更符合值语义，也能避免字符串字面量与普通数组推断不一致。

可以把它理解成一个近似的 `materialize(T)` 规则：

- `materialize(u8 const) = u8`
- `materialize(Pointer<T> const) = Pointer<T>`
- `materialize(IndexablePointer<T> const) = IndexablePointer<T>`
- `materialize(Array<T>) = Array<materialize(T)>`

直观含义是：

- 被复制出来的新值对象所在这一层，可以去掉这一层 `const`
- 数组按值复制时会继续复制元素，所以元素也继续物化
- 指针复制只复制地址值，不复制 pointee，所以 pointee 保持原样

### 5.2 基础值拷贝

例如：

```lona
var a u8 const
var b = a
```

这里发生的是：

- 一个 `u8 const` 值被复制成新的 `u8` 值对象

因此：

- `b: u8`

### 5.3 指针 / 可索引指针的第一层 `const`

例如：

```lona
var a u8 const* const
var b = a
```

这里发生的是：

- 指针值本身被复制
- pointee 没有被复制

因此允许丢掉：

- 指针这一层的 `const`

但不允许丢掉：

- pointee 的 `const`

结果是：

```lona
var b = a           // b: u8 const*
```

同样：

```lona
var a u8 const[*] const
var b = a           // b: u8 const[*]
```

### 5.4 数组按值复制

数组按值复制时，数组元素本身也被复制。

因此：

```lona
var a u8 const[4]
var b = a
```

结果是：

```lona
var b = a           // b: u8[4]
```

原因是：

- 数组这一层发生了值复制
- 每个元素这一层也发生了值复制
- 因此元素层的 `const` 也可以随复制一起去掉

这条规则也正好覆盖字符串字面量：

```lona
"1234"              // u8 const[4]
var str = "1234"    // str: u8[4]
```

### 5.5 显式类型仍然允许显式指定目标

如果用户显式写了目标类型，则允许通过按值复制得到一个更少限定的对象。

例如：

```lona
var arr u8 const[4]
var a u8 = arr(0)
```

这里的含义是：

- `arr(0)` 的类型仍然是 `u8 const`
- 但显式目标类型要求一个新的 `u8` 值
- 因此允许复制出一个新的可变 `u8`

这条规则只适用于真正的按值物化。

### 5.6 别名类类型不能越过未复制层去掉 `const`

对于指针、可索引指针和 `ref` 这一类别名语义类型，不允许通过普通赋值去掉“未发生值复制”的更深层 `const`。

例如：

```lona
var p u8 const[*]
var q = p              // q: u8 const[*]
var r u8[*] = p        // 不允许
```

因为这里没有复制 pointee bytes，只是复制了指针值本身。

否则就等于通过别名绕过了只读约束。

## 6. 结构体字段边界

### 6.1 字段不允许顶层 `const`

`const_qualifier_v0` 不把“成员不可变”建模成字段类型上的顶层 `const`。

也就是说，结构体字段不允许这类写法：

```lona
struct Bad {
    x u8 const
    p u8* const
    q u8[*] const
    a u8 const[4]
    b u8[4] const
}
```

这些写法都应拒绝。

原因是当前字段初始化模型要求：

- 字段在初始化阶段必须拥有对该字段对象本身的完整读写权限

而上面这些类型都会让字段对象本身或字段内部值失去完整写权限，因此不适合作为字段类型。

### 6.2 字段允许“指向 const”的指针

结构体字段允许这类写法：

```lona
struct View {
    p u8 const*
    q u8 const[*]
}
```

原因是：

- `u8 const*`
- `u8 const[*]`

限制的是 pointee，而不是字段对象本身。

也就是说：

- 字段本身仍然可以在初始化时被正常赋值
- 只是通过该字段看到的目标 bytes 不可写

### 6.3 数组字段上的两种 `const` 都禁止

下面两种数组字段写法都不允许：

```lona
u8 const[4]
u8[4] const
```

原因不同，但结论一致：

- `u8 const[4]`：元素不可写，字段初始化拿不到完整元素写权限
- `u8[4] const`：数组对象本身不可整体写入，字段初始化拿不到完整对象写权限

因此这两种都不适合作为字段类型。

### 6.4 未来若要支持成员不可变，应单独设计 `readonly`

如果未来要支持“成员变量外部只读、内部可写”，应采用独立的成员语义，例如：

- `readonly`

它不是类型修饰，不应与 `const` 混用。

也就是说，未来的分层应当是：

- `const`：类型系统里的不可改写视图
- `readonly`：成员声明级的可见性 / 可写性规则

`const_qualifier_v0` 只定义前者，不试图把后者塞进字段类型。

## 7. 与 `ref` 的关系

`ref` 不等于 mutable。

也就是说：

```lona
ref p u8 const[*]
```

是合法的。

它表示：

- `p` 是一个引用绑定
- 这个引用绑定所看到的类型是 `u8 const[*]`

因此通过 `p`：

- 可以读
- 可以转发
- 不能写 pointee bytes

## 8. 与字节串的关系

这份草案和 [../archive/design/string_bytes_v0.md](../archive/design/string_bytes_v0.md) 是配套的。

字节串字面量本身仍然是：

- `u8 const[N]`

但字符串常量的 bytestream 借用类型应当是：

- `u8 const[*]`

也就是说：

```lona
"1234"     // u8 const[4]
var str = "1234" // u8[4]
&"1234"    // u8 const[*]
```

原因很直接：

- 字节串字面量的 bytes 不应允许通过借用视图被改写

因此：

- `puts(&"ok")` 这种只读消费场景是自然的
- `(&"ok")(0) = 65` 这类写入应当被拒绝

## 9. ABI 与实现注意点

### 8.1 `const` 是类型系统语义，不是 ABI 语义

`const` 应进入：

- 语义类型系统
- 类型比较 / 推断
- 模块接口与增量失效

但 `const` 不应改变：

- C ABI
- native ABI
- LLVM 物理传参 / 返回表示

也就是说，它影响“能不能写”，不影响“怎么传”。

### 8.2 写入点统一检查目标类型是否可写

至少这些位置都需要检查：

- `x = y`
- `*p = v`
- `p(i) = v`
- `obj.field = v`

判断原则是：

- 当前被写目标所看到的类型节点如果是 `const`，就拒绝

## 10. 总结

一句话总结：

- `var` / `ref` 是定义修饰
- `const` 是类型修饰
- `const` 修饰左边刚形成的类型节点
- `const` 默认是 shallow 的
- 当层值拷贝允许丢弃当层 `const`
- 结构体字段禁止顶层 `const`
- 成员不可变如果以后要做，应单独设计 `readonly`
- `&"..."` 的结果应是 `u8 const[*]`
