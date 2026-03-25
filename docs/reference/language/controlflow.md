# 控制流与块语句示例

> 对应 `grammar.md` 的“3.2 语句”。

## 1. 块语句

```lona
{
    var x = 1
    x += 2
}
```

## 2. 空块

```lona
{}
```

## 3. `ret` 语句

```lona
ret
ret 1
ret a + b
```

## 4. `if` 语句

```lona
if a < b {
    ret a
}
```

条件直接跟在 `if` 后面，不要求额外的圆括号。

## 5. `if ... else ...`

```lona
if a < b {
    ret a
} else {
    ret b
}
```

`else` 也可以写到下一行：

```lona
if a < b {
    ret a
}
else {
    ret b
}
```

这里的换行规则是：

- `if cond {` 里的 `{` 必须和条件表达式写在同一行。
- `else` 可以放到下一行，也可以和前一个 `}` 之间夹空行或仅注释行。
- 但 `else` 后面不能再换行；要么直接写 `else {`，要么直接写 `else if`。

## 5.1 `else if` 链

```lona
if score > 90 {
    ret 1
} else if score > 60 {
    ret 2
} else {
    ret 3
}
```

当前 `else if` 是 `else` 后面继续跟一个 `if` 的语法糖；语义上等价于：

```lona
if score > 90 {
    ret 1
} else {
    if score > 60 {
        ret 2
    } else {
        ret 3
    }
}
```

## 6. `for` 语句

```lona
for running {
    ret
}
```

`for` 的头部在当前 grammar 中就是一个普通 `expr`，例如也可以写成：

```lona
for i < 10 {
    ret
}
```

## 7. `break` / `continue`

```lona
for running {
    if should_stop {
        break
    }
    if should_skip {
        continue
    }
}
```

- `break` 会立刻结束最近一层 `for`。
- `continue` 会立刻回到最近一层 `for` 的下一轮条件检查。

## 8. `for ... else ...`

```lona
for i < limit {
    i = i + 1
} else {
    ret 1
}
```

也可以把 `else` 写在下一行：

```lona
for i < limit {
    i = i + 1
}
else {
    ret 1
}
```

`for ... else ...` 也遵循同样的规则：

- `for cond {` 里的 `{` 必须和循环条件写在同一行。
- `else` 可以晚几行出现，但 `else` 后面必须直接跟 `{`。

说明：

- `else` 只在循环条件自然变成假时执行。
- 如果循环体里发生 `break`，则会直接跳过 `else`。
- 如果循环体里使用 `continue`，只是开始下一轮检查；只要循环最终是自然结束的，`else` 仍然会执行。
