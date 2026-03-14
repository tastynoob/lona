# 结构体声明示例

> 对应 `docs/grammer.md` 的“3.3 结构体与函数声明”。

## 1. 只有字段的结构体

```lona
struct Point {
    x i32
    y i32
}
```

## 2. 字段类型可以是自定义类型

```lona
struct Line {
    start Point
    finish Point
}
```

## 3. 结构体中可以定义方法

```lona
struct Counter {
    value i32

    def inc(step i32) i32 {
        ret self.value + step
    }
}
```

## 4. 字段与方法可以混合出现

```lona
struct Buffer {
    data i32*
    size i32

    def empty() bool {
        ret self.size == 0
    }
}
```

## 5. 当前不支持空结构体

当前 grammar 要求 `struct` 花括号里至少出现一个 `struct-stat`，因此下面这种空结构体写法不在现有文法支持范围内：

```lona
struct Empty {
}
```
