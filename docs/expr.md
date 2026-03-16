# 表达式示例

> 对应 `docs/grammer.md` 的“3.5 表达式”和“4. 运算符优先级与结合性”。

## 1. 基本值

```lona
1
3.14
"hello"
true
false
name
```

说明：

- `3.14`、字符串字面量当前已经进入词法和 AST。
- 运行时 / lowering 仍然只对其中的一部分形成闭环；未接通的部分会给出明确占位错误。
- `i32 1`、`bool(expr)` 这类 C 风格 cast 写法已经移除。

## 2. 括号表达式与括号应用

```lona
(1 + 2)
(a < b)

sum()
sum(1, 2)
factory()(2)
```

当前 `xxx(...)` 在语义上先统一看成“括号应用”。
具体是函数调用、函数指针调用，还是未来的数组访问 / 其它重载行为，由后续语义阶段决定。

## 3. 显式函数取指针

```lona
foo&<>
sum&<i32, i32>
```

这里的 `name&<...>` 表示“按形参类型列表取得顶层 `def name(...)` 的函数指针”。
当前实现里，`&<` 是一个专用起始符，因此写法应保持成连续的 `&<`。
取到的结果就是普通函数指针值，因此后续可以继续写 `cb(1)`、`make_cb()(1)` 这类间接调用。

## 4. 成员访问与链式调用

```lona
point.x
user.profile.name
obj.method(1)
math.inc(1)
```

其中 `math.inc(1)` 这种形式表示调用 imported module `math` 暴露出来的顶层函数。

## 5. 元组与数组初始化

```lona
var pair <i32, bool> = (1, true)
var matrix i32[4][5] = {{}}
```

说明：

- 元组字面量和数组初始化现在已经能进入 AST。
- 当前 milestone 只完成了 frontend 接线；tuple / fixed array 的完整语义和 lowering 仍是后续工作。

## 6. 一元运算

```lona
!flag
~mask
-value
+value
&value
*ptr
```

注意：`foo&<i32>` 这种显式函数取指针不是普通一元 `&`，而是单独的表达式形式。

## 7. 二元与赋值运算

```lona
a + b
a - b
a * b
a / b
a % b

a < b
a > b
a <= b
a >= b
a == b
a != b

a & b
a ^ b
a | b
a && b
a || b

x = 1
x += 1
x -= 1
x *= 2
x <<= 1
point.x = 1
*ptr = 42
```

说明：

- 这一轮已经把 C 风格常见运算符接进了 lexer / parser / precedence。
- 并不是所有运算符都已经有完整语义；未完成的项会在语义阶段给出明确占位错误，而不是模糊的 generic unsupported。

## 8. 优先级示例

```lona
a + b * c
(a + b) * c
obj.value + getX()
a == b + 1
a | b && c
```

按当前优先级声明，`* / %` 高于 `+ -`，移位高于比较，位运算高于逻辑运算，成员访问与括号应用也高于普通二元运算。显式函数取指针 `name&<...>` 本身作为一个单独的基础值参与后续组合。
