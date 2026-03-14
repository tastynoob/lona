# 程序结构示例

> 对应 `docs/grammer.md` 的“3.1 程序结构”和“3.2 语句”。

## 1. 顶层可以直接放语句

```lona
var answer = 42
answer += 1
```

## 2. 顶层可以放函数声明

```lona
def id(x i32) i32 {
    ret x
}
```

## 3. 顶层可以放结构体声明

```lona
struct Point {
    x i32
    y i32
}
```

## 4. 顶层允许空行

```lona

var x = 1

def inc(v i32) i32 {
    ret v + 1
}
```

## 5. 块语句可以包含多条语句

```lona
{
    var x = 1
    x += 2
    ret x
}
```

## 6. 空块是合法的

```lona
def noop() {}
```

## 7. 顶层存在一个特殊兼容形式

```lona
i32
u32 bool
```

这类写法来自 `bare_type_seq NEWLINE`，当前可以通过解析，但不会形成常规的声明语义。
