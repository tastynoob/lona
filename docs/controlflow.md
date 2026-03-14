# 控制流与块语句示例

> 对应 `docs/grammer.md` 的“3.2 语句”。

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
