# Inline v0

> 这是一份 `inline` 语言特性的第一版草案。
> 这里的 `inline` 表示一种新的编译期常量绑定语义：被 `inline` 定义的名字，其值必须在编译期已知。
> 这版 `inline` 只表达“编译期常量绑定”，不表达文本替换，也不表达函数内联。
> 第一版故意只覆盖原生标量与指针，避免一上来就把它做成完整 CTFE、值泛型或常量解释器系统。
>
> 实现状态更新：
>
> - 局部 `inline` 与顶层 `inline` 都已经接通
> - 顶层 `inline` 可以被同模块后续函数和 importer 通过 `file.xxx` 访问
> - 顶层 `inline` 不会物化成独立运行时全局符号

## 1. 目标与范围

`inline_v0` 先定义这几件事：

- `inline` 是定义修饰关键字
- `inline` 绑定的值必须在编译期已知
- `inline` 绑定默认只读、单次初始化
- `inline` v0 只接受原生标量与指针类型
- `inline` 绑定的初始化器必须来自受限的 inline expression 子集
- `inline` v0 按语义常量处理，不按文本替换处理
- `inline` 绑定的类型确定和值求值都必须在编译期完成

这份草案**不**定义：

- 用户自定义 CTFE / constexpr 函数
- 宏式文本替换
- 值级泛型参数
- struct / tuple / fixed array 的 inline 值
- `inline global`
- `inline def`
- inline 参数、inline 返回值、inline struct 字段
- “编译器自动推导任何纯表达式都是编译期常量”的隐式系统

也就是说，这里讨论的是：

- 如何给 `lona` 先落地一版明确、低歧义、易实现的编译期常量绑定语义

## 2. 核心原则

### 2.1 `inline` 是定义语义

`inline` 应该和：

- `var`
- `const`
- `ref`
- `global`

处在同一层级，属于“名字怎么定义”的问题。

v0 推荐写法是：

```lona
inline size i32 = 4
inline scale f64 = 0.5
inline msg u8 const[*] = "hello"
```

- 绑定的普通值类型仍然是 `i32`、`f64`、`u8 const[*]`
- “编译期已知”是这个绑定和由它派生出来的表达式的附加语义

### 2.2 `inline` 独立成一种绑定形态

在 v0 里，`inline` 自身就意味着：

- 单次初始化
- 之后不可重新赋值
- 不承诺有稳定存储地址

v0 采用这组规则：

- `const` 表示运行时只读绑定
- `inline` 表示编译期常量绑定

语法上只接受单独的 `inline name ...` 形态。

### 2.3 `inline` 先做显式能力，不做隐式魔法

v0 不尝试让编译器自动推导“这个运行时值其实也能算常量”。

相反，规则是：

- 只有显式写了 `inline` 的绑定，才进入 inline 语义
- 只有符合 inline expression 子集的表达式，才能初始化 `inline` 绑定

这样做的好处是：

- 诊断边界清楚
- 不需要引入大规模数据流常量传播才能定义语言语义
- 后续要扩展到数组维度、值泛型时，也有稳定的值域可以复用

### 2.4 `inline` 初始化必须在编译期完成确定

`inline` 的初始化检查包含两件事：

- 绑定类型必须在编译期确定
- 初始化表达式的值必须在编译期求得

这两件事缺一不可。

也就是说：

- 仅仅“目标类型写出来了”还不够，初始化器本身也必须属于 inline expression
- 仅仅“表达式最后可能能算出一个常量”还不够，编译器必须在语义阶段就把它确定下来

`inline` 的语义边界应该是：

- 进入 `inline` 绑定的表达式，在编译期就已经是 fully-resolved constant expression

而不是：

- 先按普通运行时表达式通过语义检查，再期待后端或优化器把它折叠成常量

### 2.5 `inline` 是语义常量，不是文本替换

`inline` 绑定的语义模型是：

- 编译器先按普通语言规则解析、类型检查并验证初始化表达式
- 绑定成功后，这个名字在后续使用点上可作为编译期常量参与求值
- 使用点拿到的是“这个绑定的常量值”，不是源码片段的重新展开

因此这版 `inline` 不包含这些语义：

- 宏式 token 替换
- 按使用点重新解析初始化器
- 通过名字展开绕开作用域、类型检查或求值顺序

这条边界很重要，因为 v0 需要的是：

- 一个可检查、可缓存、可用于后续常量求值扩展的语义常量模型

而不是：

- 一套新的预处理或宏系统

## 3. 语法建议

### 3.1 局部绑定语法

推荐在现有 `var-def` 里新增两种形态：

```ebnf
var-def ::= ...
          | "inline" IDENT type-name "=" expr
          | "inline" IDENT "=" expr
```

对应示例：

```lona
inline size i32 = 4
inline scale = 0.25
inline ok bool = true
inline msg = "hello"
```

这里：

- `inline name T = expr` 走显式类型检查
- `inline name = expr` 走现有推断规则

如果初始化器本身无法独立推断类型，则仍然要求显式类型：

```lona
inline p i32* = null   // 合法
inline p = null        // 拒绝
```

### 3.2 `inline name = expr` 的推断边界

`inline name = expr` 只有在下面两件事都成立时才合法：

- `expr` 本身属于 inline expression 子集
- `expr` 的值类型可以在编译期唯一确定

例如：

```lona
inline size = 4
inline scale = 0.25
inline ok = true
inline msg = "hello"
```

这些都可以直接在编译期确定类型和值。

而下面这些需要拒绝：

```lona
inline p = null
inline value = runtime_fn()
inline size = runtime_var
```

对应原因分别是：

- `null` 缺少目标指针类型
- 函数调用不属于 v0 的 inline expression
- 运行时绑定不是 inline expression

因此 `inline name = expr` 不应理解成“先照常推断，再看能不能顺手折叠”，而应理解成：

- 只有当类型推断和值求值都能在编译期完成时，省略类型才成立

### 3.3 v0 接通局部与顶层 `inline`

第一版最终接通了两种位置：

- 函数体 / block 内的局部 `inline` 绑定
- 文件顶层的 `inline` 绑定

例如：

```lona
def encode() i32 {
    inline header i32 = 4
    inline scale f64 = 0.25
    ret header
}
```

顶层 `inline` 的额外规则是：

- 进入模块接口，可以被 importer 通过 `file.xxx` 读取
- 在同模块里对后续函数和顶层语句按普通名字可见
- 不拥有独立运行时全局存储

这版**仍然不**接通：

- `inline global name T = expr`
- `global inline name T = expr`
- `def foo(inline x i32)`
- `def foo() inline i32`
- `struct S { inline value i32 }`

这些位置会分别引入额外复杂度：

- `inline global` 会把“编译期常量绑定”和“独立运行时全局存储”重新混在一起
- inline 参数 / 返回值会带来值级特化与实例化问题
- inline 字段会把布局系统和内存模型卷进来

因此 v0 最合理的落地面是：

- 先把局部 / 顶层 inline 常量绑定做干净

## 4. 允许的值类型

`inline` 绑定出来的值仍然要落在受限类型集里。

### 4.1 内建标量

允许：

- `u8`、`i8`
- `u16`、`i16`
- `u32`、`i32`
- `u64`、`i64`
- `int`、`uint`
- `f32`、`f64`
- `bool`

例如：

```lona
inline width i32 = 64
inline eps f64 = 0.0001
inline ready bool = true
```

### 4.2 顶层原始指针与可索引指针

允许：

- `T*`
- `T[*]`

其中 `T` 本身仍然可以带普通限定，例如：

```lona
inline p i32* = null
inline bytes u8 const[*] = "lona"
```

这里：

- `inline` 约束的是绑定值本身在编译期已知
- `const` 约束的是 pointee / element 是否可写

也就是说：

- `inline bytes u8 const[*] = "lona"` 表示这个绑定保存的是编译期已知的只读字节视图

### 4.3 v0 明确拒绝的值类型

这版不接受：

- 固定维数组
- tuple
- struct
- `Trait dyn`
- applied generic type
- 顶层函数指针

例如：

```lona
inline row i32[4] = {1, 2, 3, 4}     // 拒绝
inline pair <i32, bool> = (1, true)  // 拒绝
inline point Point = Point()          // 拒绝
```

函数指针之所以先排除，是因为它虽然表面上也像“地址常量”，但它会立刻碰到：

- 顶层函数实体引用
- 跨模块符号可见性
- 泛型函数实例化

这条线适合作为后续扩展。

## 5. Inline Expression 子集

`inline` 绑定的初始化器必须来自受限的 inline expression 子集，并且该表达式必须在编译期完成类型确定和值求值。

### 5.1 直接字面量

允许：

- 整数字面量
- 浮点字面量
- `true` / `false`
- 字符字面量
- `null`
- 字符串字面量

例如：

```lona
inline answer i32 = 42
inline ready bool = true
inline empty i32* = null
inline msg = "hi"
```

字符串字面量仍然按现有规则推断成 `u8 const[*]`。

### 5.2 已有 inline 绑定

允许引用先前已经建立的 inline 绑定：

```lona
inline a i32 = 4
inline b i32 = a + 1
```

普通运行时绑定不能反向“升级”为 inline：

```lona
const x i32 = 4
inline y i32 = x   // 拒绝
```

这里的拒绝原因不是“值看起来像常量”，而是：

- `x` 的语义类别是普通运行时绑定，不是 inline 绑定
- `inline` 初始化不能依赖运行时绑定再做二次猜测

### 5.3 纯内建一元 / 二元运算

允许在 inline 值上执行现有内建纯运算，只要：

- 操作数都是 inline
- 运算本身已有稳定运行时语义
- 结果类型仍然属于允许的值类型集

例如：

```lona
inline a i32 = 1 + 2 * 3
inline b i32 = (1 << 5) | 3
inline c bool = a > 4
inline d f64 = 1.0 + 2.0
```

### 5.4 显式 `cast[T](expr)`

允许：

- 源操作数是 inline expression
- 目标 `T` 也是合法的 inline 绑定值类型
- 该 `cast` 本来就是运行时允许的显式静态转换

例如：

```lona
inline bits u32 = cast[u32](255)
inline scale f64 = cast[f64](3)
```

### 5.5 `sizeof`

如果 `sizeof[T]()` 或 `sizeof(expr)` 在当前语义里可求得固定结果，则允许进入 inline 子集：

```lona
inline bytes int = sizeof[i64]()
```

### 5.6 指针常量

对于指针型 inline 绑定，v0 先建议支持：

- `null`
- 字符串字面量
- 取 extern / global 符号地址

例如：

```lona
inline msg u8 const[*] = "lona"
```

而下面这些先不进入 v0：

- `&local`
- `&arr(0)`
- `&point.x`
- 任意函数调用返回的指针

核心原则是：

- v0 只接稳定、无需运行时求值的 relocatable constant

## 6. 使用规则

### 6.1 `inline` 绑定读出来的仍是 inline expression

读取一个 inline 绑定，结果仍然是 inline expression：

```lona
inline a i32 = 4
inline b i32 = a + 1
```

也就是说：

- inline 已知性会沿着受支持的纯表达式传播
- 它是一种额外语义属性
- 使用点消费的是常量值语义，不是初始化器源码的再次展开
- 只要某一步传播需要落回运行时求值，这条 inline 链就应当中断并报错

### 6.2 inline 可以自然用在普通运行时上下文

`inline` 绑定的值类型仍然是普通 `T`，因此它可以直接用于普通运行时位置：

```lona
inline size i32 = 16
var runtime i32 = size
```

这里直接按普通 `T` 使用即可。

### 6.3 普通运行时值不能初始化 inline 绑定

反方向不允许：

```lona
var runtime i32 = 16
inline size i32 = runtime   // 拒绝
```

### 6.4 inline 绑定不承诺有稳定地址

因为 inline 绑定的目标是“直接内联到使用点”，v0 不承诺它一定拥有独立存储槽位。

因此下面这些写法都应拒绝：

```lona
inline size i32 = 16
var p i32* = &size      // 拒绝
take(ref size)          // 拒绝
```

这条规则很重要，因为它能避免：

- 为了 inline 常量绑定强行物化 alloca
- `ref` / 地址语义把“编译期常量值”重新拖回运行时存储模型

### 6.5 inline 绑定不能重新赋值

```lona
inline width i32 = 64
width = 32   // 拒绝
```

这是 `inline` 绑定语义的一部分。

## 7. 与现有语义的关系

### 7.1 和 `const`

`const` 是运行时只读绑定。

```lona
const size i32 = 16
```

它表示：

- 这个名字绑定到一个只读值
- 但这个值未必编译期已知
- 编译器也不必把它当作可传播的编译期常量

而：

```lona
inline size i32 = 16
```

表示：

- 这个绑定必须由 inline expression 初始化
- 后续读取它时可以继续参与 inline expression 求值
- 编译器不承诺它具有可寻址的稳定存储

### 7.2 和指针

`inline p T* = expr` / `inline p T[*] = expr` 的含义是：

- 这个指针值本身在编译期已知

它不意味着：

- 允许任意 pointee 读取也进入 inline 域
- 允许 `*p` 自动得到某个 inline 值

也就是说：

- 指针值可内联
- 指针解引用仍然是普通运行时内存访问

### 7.3 和未来值泛型 / 数组维度

这份草案故意不直接定义值泛型或 const generic。

但它的价值在于先给未来功能准备一个稳定值域：

- 未来如果要做数组维度编译期常量
- 未来如果要做 inline 参数或值泛型

都可以复用这版“inline binding + inline expression”的基础规则。

## 8. 诊断建议

推荐给出 targeted diagnostic。

例如：

- `inline` 目前只允许出现在局部或顶层绑定
- `inline` 绑定的值类型必须是原生标量或指针
- `inline` 初始化的类型和值都必须在编译期确定
- 这个初始化表达式不是 inline expression
- 运行时值不能初始化 inline 绑定
- inline 绑定没有稳定地址，不能取 `&`
- inline 绑定不能作为 `ref` 实参传递
- `inline` 不能和 `const` / `var` / `ref` 叠写

## 9. 实现建议

如果后续按这份草案落地，建议按下面顺序推进：

1. parser / AST 接通 `inline` 作为新的定义修饰关键字
2. sema 里增加 `isInlineBinding` / `isInlineExpr` / `materializeInlineValue`
3. 先接通局部绑定，再把顶层 `inline` 接到模块接口和缓存失效模型
4. codegen 对标量 / 指针 inline 绑定避免分配存储槽位，直接按 LLVM constant / immediate 使用
5. diagnostics 单独做 targeted error，避免把所有失败都归为普通初始化错误

这样做的好处是：

- 不需要把 `inline` 塞进类型系统
- 不需要先引入完整解释器
- 只需要把顶层 `inline` 纳入模块接口哈希
- 不需要先碰值级实例化
- 先把最小值域和最小语义边界做稳

## 10. 非目标与后续扩展

更适合作为后续版本的方向包括：

- `inline global`
- `inline def`
- inline 参数 / inline 返回值
- 顶层函数指针的 inline 常量
- fixed array / tuple / struct 的组合 inline 常量
- 基于 inline 值的数组维度与值泛型
- 用户可定义的常量求值函数

推荐的策略是：

- 先把 v0 做成“局部显式 compile-time-known scalar/pointer binding”
- 未来如果要支持 `inline def`、内联函数提示或其他同名特性，再把它们作为独立语义单独设计
- 再按真实需求逐步扩大值域和出现位置
