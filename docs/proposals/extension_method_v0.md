# Extension Method v0

> 这是一份面向 `lona` 的 extension method 草案。
> 目标不是把现有 ordinary method / trait method 推翻重来，而是在当前 `obj.method(...)`、`Trait.method(&obj, ...)`、一层 implicit deref 和模块导入模型之上，先收口一版可实现的扩展方法语义。

## 1. 状态与范围

`extension_method_v0` 只定义：

- extension method 的声明语法
- body 内隐式 `self` 绑定
- `obj.method(...)` 的查找优先级
- 当前模块与直接 `import` 模块的 extension 可见性
- borrowed receiver 的匹配规则

这份草案**不**定义：

- extension method 的显式模块级解歧语法
- generic receiver
- tuple / array / `Trait dyn` / function pointer receiver
- pointer value 自身的独立 extension method 语法
- 把 proposal 直接提升为 reference 文档

换句话说，这里讨论的是：

- 如何在当前语言里增加一层“可导入、可见、但不抢占 inherent / trait method”的扩展方法机制

而不是：

- 如何一步到位得到一套带完整解歧、泛型 receiver 和所有运行时形态的最终系统

## 2. 设计目标

这版设计希望同时满足四个约束：

- `1.tobits()` 这类写法在概念上更接近“内建 extension method”，而不是特殊注入成员。
- 不改变现有 ordinary method 和 trait method 的优先级边界。
- 导入某个模块后，它声明的 extension method 可以在当前模块生效，但这种生效范围不继续向下游 importer 传播。
- 复合类型不再提供 value receiver 版本，避免 struct 按值 receiver 和当前 hidden pointer receiver 成本模型冲突。

## 3. 声明语法

建议的声明形态是：

```lona
def i32.to_s() Str {
    ...
}

def (i32 const*).is_zero() bool {
    ret *self == 0
}

def (Point const*).len() f32 {
    ...
}

def (Point*).translate(dx f32, dy f32) {
    ...
}
```

规则：

- `def T.method(...)` 表示 value receiver extension method。
- `def (T const*).method(...)` 表示只读 borrowed receiver extension method。
- `def (T*).method(...)` 表示可写 borrowed receiver extension method。
- 裸写 `T` 只用于简单的非指针 receiver。
- 任何指针、`const*`、imported type、applied generic type 都应写成括号形式。

## 4. receiver 允许范围

v0 建议允许下面两类 receiver：

- builtin scalar type 的 value receiver，例如 `i32`、`bool`、`f32`
- concrete type 的 borrowed receiver，例如 `T const*` / `T*`

其中再额外收一条硬规则：

- 复合类型不支持 value receiver

也就是说：

- `def i32.method(...)`：允许
- `def (i32 const*).method(...)`：允许
- `def (i32*).method(...)`：允许
- `def Point.method(...)`：不允许
- `def Box[i32].method(...)`：不允许
- `def (Point const*).method(...)`：允许
- `def (Point*).method(...)`：允许

这样做的原因是：

- 结构体当前 ordinary method 本质上已经是 hidden `Self const*` / `Self*` receiver
- 如果 extension method 再允许 struct value receiver，会引入按值拷贝和 borrowed receiver 并存的双语义
- v0 先统一成“复合类型只走 borrowed receiver”更容易保持语义和实现收敛

这里再额外收一条排除规则：

- 可索引指针 `T[*]` 不支持 extension method

原因是：

- `T[*]` 当前语义更接近“可索引的数组视图”，而不是 ordinary receiver object
- 数组本身不支持 extension method
- 因此 `T[*]` 也不应通过隐式转换退回成 `T*` receiver 去参与 extension method 匹配

## 5. `(T*)` 的含义

`(T*)` 在 v0 里保留给 borrowed receiver。

它的含义是：

- extension body 看到的是一个可写 `self T*`
- 调用点可以来自一个可写的 `T` 值
- 也可以来自一个 `T*`，并沿用现有 dot-call 的一层 implicit deref 规则重试解析

这也意味着：

- v0 里暂时**不**提供“pointer value 自身的 exact extension method”语法

原因是如果 `(T*)` 同时既可能表示：

- “receiver 是一个指针值”

又可能表示：

- “receiver 是一个借用的 `T`”

那么 `ptr.method()` 的解析会立刻失去确定性。

因此，v0 的明确取舍是：

- `(T*)` 只表示 borrowed receiver
- pointer value 自身如果未来要有独立 extension method，需要另设不冲突的语法

## 6. body 语义

extension method body 内自动注入隐式 `self` 绑定。

例如：

```lona
def i32.double() i32 {
    ret self + self
}

def (Point const*).len2() i32 {
    ret self.x * self.x + self.y * self.y
}
```

规则：

- `def i32.double()` 里的 `self` 类型是 `i32`
- `def (Point const*).len2()` 里的 `self` 类型是 `Point const*`
- `def (Point*).translate(...)` 里的 `self` 类型是 `Point*`
- 用户不能再显式声明同名参数 `self`

extension method 不使用 `set def`。

receiver 可写性完全由下面两种拼写表达：

- `T const*`
- `T*`

## 7. 调用语法

调用端仍然统一使用：

```lona
value.method(...)
```

v0 不引入新的调用符号，也不把裸 selector 当成调用。

也就是说：

- `value.method(...)`：调用
- `value.method`：仍然不是合法的可存储方法值

## 8. 方法命名空间与查找

extension method 和 ordinary inherent method 在 v0 里共享同一个“普通方法命名空间”。

这意味着：

- `Type.foo` ordinary method 和 `Type.foo` extension method 不能同时存在
- 同一个 receiver type 的两个同名 extension method 也不能同时可见
- 这类冲突不做重载分派，直接报错

这样做的原因是：

- ordinary method 和 extension method 最终都通过 `obj.method(...)` 进入同一条普通点调用语法
- 如果允许同名共存，就必须再发明一套显式解歧语法
- v0 明确不打算把普通点调用变成重载系统

在没有命名冲突时，`obj.method(...)` 的查找顺序建议固定为：

1. 先看普通方法命名空间，也就是 inherent method + visible extension method
2. 如果普通方法命名空间没有命中，再看 trait method
3. 如果前两层都未命中，再看内建 bit-copy helper，例如当前的 `tobits` / `u8[N].toXXX()`

因此：

- ordinary method 和 extension method 不通过优先级抢占彼此，而是通过“同名即冲突”提前收口
- trait method 仍然保持在普通方法命名空间之后
- injected-member / bit-copy helper 继续留在最后一层

这也意味着：

- 如果某个名字同时存在于普通方法命名空间和 trait 方法命名空间，默认仍然优先调用普通方法
- 这条规则和当前 inherent method 对 trait fallback 的遮蔽关系保持一致
- 如果用户需要调用同名 trait 方法，应继续写显式 trait 路径，例如 `obj.Trait.method(...)` 或 `Trait.method(&obj, ...)`

同时，trait 方法和普通方法在符号命名层仍然是分开的：

- trait 方法继续沿用现有 trait 专属符号修饰
- ordinary method 和 extension method 继续沿用普通方法符号修饰

因此：

- trait method 和普通方法即使同名，也不会因为链接符号名相同而冲突
- 这类同名问题只在语义查找顺序上处理，不是链接层冲突

## 9. receiver 匹配顺序

当进入 extension method 查找后，建议按下面顺序匹配：

1. 先做 value receiver 匹配
2. 如果 value receiver 未命中，再做 borrowed receiver 匹配

### 9.1 参考现有 `const` 语义

receiver 匹配不再用“短类型 / 长类型”这种泛化说法描述。

更准确的说法是：

- value receiver 匹配遵循“按值物化会丢掉当层 `const`”的现有规则
- borrowed receiver 匹配遵循“可以增加 `const` 视图，不能丢掉已有 `const`”的现有规则

也就是说：

- 按值匹配允许忽略最外层 `const`
- 借用匹配只允许 `T* -> T const*` 这种加 `const` 方向
- 借用匹配不允许 `T const* -> T*`

### 9.2 value receiver 匹配

value receiver 匹配只看“当前表达式按值物化后的类型”。

这里再收一条规则：

- 按值匹配时忽略顶层 `const`

原因是按值调用会发生拷贝，而当前语言的值物化会丢掉当层 `const` 视图。

因此：

- `i32` 可以匹配 `def i32.method(...)`
- `i32 const` 也可以匹配 `def i32.method(...)`

但 value receiver 匹配还有两个硬边界：

- pointer-typed expression 不会通过 implicit deref 去匹配 value receiver
- `T[*]` expression 不参与 value receiver extension lookup

因此：

- `var p i32*`
- `p.method()`

不会去匹配：

- `def i32.method(...)`

### 9.3 borrowed receiver 匹配

如果 value receiver 未命中，再尝试 borrowed receiver。

borrowed receiver 分两种来源：

- 当前 receiver 本来就是某个 pointer type
- 当前 receiver 是一个可借用的非指针值，此时允许隐式取地址后再匹配
- 当前 receiver 是 concrete struct 临时值，此时允许像 ordinary method 一样先物化临时槽位，再按 borrowed receiver 匹配

这里也要明确一个边界：

- 标量字面量和其它非结构体临时值不会为 borrowed receiver 自动物化存储

因此：

- `Point(x = 1, y = 2).len()` 可以去匹配 `def (Point const*).len()`
- `1.method()` 不会去匹配 `def (i32 const*).method()`

这里的规则是：

- borrowed receiver 不能忽略指针上的 `const` 约束
- 可写 borrowed receiver `T*` 只接受可写来源
- 只读 borrowed receiver `T const*` 可以接受可写或只读来源
- `T[*]` 不参与 borrowed receiver 匹配，也不通过隐式转换退回 `T*`

因此：

- `T` 可以进一步匹配 `T const*` 或 `T*`
- `T const` 只能进一步匹配 `T const*`
- `T const` 不能匹配 `T*`
- `T*` 可以匹配 `T const*`
- `T const*` 不能匹配 `T*`
- `T[*]` 既不能匹配 `T const*`，也不能匹配 `T*`

### 9.4 例子

```lona
var a i32
var p i32*
var c i32 const = 1
```

匹配行为应是：

- `a.method()` 先匹配 `def i32.method(...)`
- 只有当 value receiver 未命中时，`a.method()` 才继续尝试 `def (i32 const*).method(...)` 或 `def (i32*).method(...)`
- `p.method()` 只能进入 pointer receiver 匹配；它不能去匹配 `def i32.method(...)`
- `c.method()` 可以匹配 `def i32.method(...)`
- `c.method()` 可以进一步匹配 `def (i32 const*).method(...)`
- `c.method()` 不能匹配 `def (i32*).method(...)`

这样：

- 非指针值会优先尝试 value receiver
- borrowed receiver 只作为 value receiver 之后的后备路径
- pointer-typed receiver 不会因为 implicit deref 而退回 value receiver 匹配
- borrowed receiver 的临时值物化只为 concrete struct 临时值开放

但这条路径的稳定前提仍然是：

- `(T*)` 表示 borrowed receiver，而不是 pointer value receiver

## 10. import 可见性

extension method 的可见集合只包含：

- 当前模块自己声明的 extension method
- 当前模块直接 `import` 的模块里声明的 extension method

明确不包含：

- 间接导入模块的 extension method
- 自动 re-export 的 extension method

例如：

- `A` 定义 extension method
- `B` `import A`
- `C` 只 `import B`

那么在 v0 里：

- `B` 可以看到 `A` 的 extension method
- `C` 不会因为 `import B` 自动看到 `A` 的 extension method

这条规则的目的就是把 extension method 的生效范围限制在“当前模块的直接依赖集合”里，避免传递式污染 `obj.method(...)` 的绑定结果。

## 11. 冲突与诊断

如果普通方法命名空间里出现多个同名候选，应直接报冲突错误。

例如：

- `Point` 自己已经定义了 ordinary method `read`
- 当前模块又引入了 `def (Point const*).read()`

那么：

- 应直接报命名冲突，而不是靠调用点分派

再例如：

- 当前模块直接 `import` 了两个都提供 `def i32.to_s()` 的模块

那么：

- 当前模块里的 `i32.to_s` extension method 集合也应视为冲突

v0 的收口方向是：

- 普通方法命名空间里不允许同名共存
- 不提供新的显式解歧语法
- 冲突要么通过重命名解决，要么通过删掉冲突 import 解决

这里要区分“符号层不冲突”和“语义层仍然冲突”：

- builtin receiver 的 extension method 也应沿用普通方法的符号命名策略
- emitted symbol 应继续携带定义模块身份

因此：

- 两个不同模块里各自定义的 `i32.to_s` 不会在链接层硬撞符号
- 但如果当前模块直接 `import` 了它们两个，那么当前模块里的 `1.to_s()` 仍然必须报语义冲突

原因不是后端分不清，而是：

- 当前语言没有显式写出“调用某个模块里的同名 extension method”的稳定语法
- 因而这类重名在当前版本只能提前报错
- 未来如果增加显式 extension 限定调用语法，再单独放宽这条限制

## 12. 与当前 injected member 的关系

当前实现里的：

- `value.tobits()`
- `u8[N].toXXX()`

仍然是专门的 injected-member / bit-copy helper 通道，而不是通用 extension method 机制。

这份 proposal 的目标不是立即改写 reference 文档，而是先把未来收口方向写清楚：

- 在概念模型上，这类能力更适合作为“内建 extension method”
- 在实现上，v0 仍允许它们暂时留在现有 injected-member 分支里
- 真正迁移时，再把它们并入统一的 extension method 解析层

## 13. 非目标

这份草案当前不试图解决：

- 泛型 receiver
- trait bound 驱动的 extension method
- tuple / array / dyn trait receiver
- pointer value 自身的 exact extension method
- extension method 作为可存储函数值
- 模块级显式解歧语法

另外再加一条能力边界：

- generic body 里的 `T.method(...)` 不考虑 extension method

也就是说：

- unconstrained `T` 不能通过 extension method 获得额外成员能力
- `[T Hash]` 这类 bounded generic 也只允许调用 bound trait 提供的方法
- `T` 的具体实例化类型即使在某个模块里拥有 extension method，也不会回流影响 generic body 的能力判断

因此，`extension_method_v0` 的核心收口可以概括为一句话：

- extension method 是一层与 inherent method 共享普通方法命名空间、只在当前模块和直接导入模块内可见、位于 trait fallback 之前、并且对复合类型只开放 borrowed receiver 的点调用扩展机制
