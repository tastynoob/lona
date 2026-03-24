# 结构体声明示例

> 对应 `grammer.md` 的“3.3 结构体与函数声明”。

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

说明：

- 当前方法接收者 `self` 隐式按引用传递。
- 在方法体里修改 `self` 的字段，会直接修改调用方对象。
- 方法调用语法不要求在接收者位置额外写 `ref`。
- 如果接收者本身是临时值，编译器会在调用点先物化一个临时槽位，再把它作为 `ref self` 传入；因此 `Vec2(1, 2).normalize()` 这类写法是允许的。

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

## 5. 空结构体的当前边界

当前 parser 接受“带换行的空结构体体”，例如：

```lona
struct Empty {
}
```

更精确地说：

- `{` 与 `}` 之间至少要出现一个换行或一个 `struct-stat`。
- 因此多行空体目前可解析。
- 但单行空体 `struct Empty {}` 仍然不在当前 grammar 支持范围内。
