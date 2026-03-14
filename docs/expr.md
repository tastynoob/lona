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

## 2. 类型前缀值

```lona
i32 1
bool true
str "hello"
```

这一路径对应：

```ebnf
single-value ::= BuiltinType typed-value-op
```

## 3. 括号表达式

```lona
(1 + 2)
(a < b)
```

## 4. 函数调用

```lona
sum()
sum(1, 2)
getValue(a, b + 1)
```

## 5. 成员访问与链式调用

```lona
point.x
user.profile.name
obj.method(1)
factory()(2)
```

## 6. 一元运算

```lona
!flag
~mask
-value
+value
&value
*ptr
```

当前 grammar 中，一元运算直接作用于 `single-value`，因此复杂嵌套时通常需要括号，例如：

```lona
!(!flag)
*(*ptr)
```

## 7. 二元运算

```lona
a + b
a - b
a * b
a / b
a < b
a > b
a & b
a == b
a != b
```

## 8. 赋值表达式

```lona
x = 1
x += 1
x -= 1
point.x = 1
*ptr = 42
```

## 9. 优先级示例

```lona
a + b * c
(a + b) * c
obj.value + getX()
a == b + 1
```

按当前优先级声明，`*`/`/` 高于 `+`/`-`，成员访问与调用也高于普通二元运算。
