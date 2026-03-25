# 结构体声明示例

> 对应 `grammar.md` 的“3.3 结构体与函数声明”。

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

## 5. 空结构体

当前 parser 同时接受多行空体和单行空体，例如：

```lona
struct Empty {
}

struct Marker {}
```

更精确地说：

- 空结构体会形成一个零字段的普通结构体类型。
- 多行空体和单行空体现在都可解析。
- 后续如果需要字段或方法，仍然按普通结构体体语法扩展即可。

## 6. opaque `#[extern] struct`

```lona
#[extern]
struct FILE
```

说明：

- 这是当前稳定可用的 opaque C 类型声明入口。
- `#[extern] struct FILE` 不暴露字段，也不能按值构造或按值定义变量；当前只支持把它放在指针位置上使用，例如 `FILE*`。
- parser 复用了普通结构体入口，因此还能吃下 `#[extern]` 标记后的 `struct FILE { ... }` 这类写法；但语义阶段会拒绝，用户应把它视为“无结构体体的 opaque 声明”。

## 7. `#[repr "C"] struct`

```lona
#[repr "C"]
struct Point {
    x i32
    y i32
}
```

说明：

- 这是当前稳定可用的 C-compatible 结构体声明入口。
- 当前语义层只接受 `#[repr "C"]`，不接受其它 repr 名称。
- `#[repr "C"] struct` 的字段类型必须也是当前 C FFI v0 支持的那一小部分类型；不兼容字段会在语义阶段报 targeted diagnostic。
- 当前 FFI v0 里，`#[repr "C"] struct` 主要按指针跨边界使用；按值传参 / 返回仍未开放。
- 更细的 FFI 限制见 [../runtime/c_ffi.md](../runtime/c_ffi.md)。
