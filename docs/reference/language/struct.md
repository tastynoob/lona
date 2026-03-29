# 结构体声明示例

> 对应 `grammar.md` 的“3.3 结构体与函数声明”。
> 本文讲普通 `struct` 语义，以及 bodyless `struct Name` 这种 opaque 声明。`#[repr "C"] struct` 的 C 边界规则见 [../runtime/c_ffi.md](../runtime/c_ffi.md)。
> `set`、字段投影、receiver 可写性和字段 `const` 边界的统一规则见 [mutability.md](mutability.md)。

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

## 3. 嵌入字段与成员提升

```lona
struct Inner {
    set value i32

    def read() i32 {
        ret self.value
    }
}

struct Outer {
    _ Inner
    value i32
}
```

说明：

- `_ T` 声明一个嵌入字段。它在语义上仍然是一个真实存在的结构体字段，只是源码里不直接写字段名。
- 这个真实字段的显式访问名默认取嵌入类型的最后一个标识符；例如 `_ Inner` 的显式访问名是 `Inner`，`_ dep.Inner` 的显式访问名也是 `Inner`。
- 因此 `obj.Inner`、`obj.Inner.value`、`obj.Inner.read()` 都是合法的显式访问写法。
- selector lookup 同时支持成员提升：如果 `Outer` 没有自己的 `name`，而某条最短嵌入路径上唯一存在 `name`，那么 `obj.name` 会被当作那条显式路径的简写。
- 直接成员优先于 promoted member；如果同一最短深度上存在多个候选，则 `obj.name` 会报歧义错误，此时需要改写成 `obj.A.name`、`obj.B.name` 这类显式路径。
- 嵌入字段参与真实布局和构造。也就是说 `Outer(Inner = Inner(...), value = 1)` 合法，而 promoted member 目前不参与外层构造参数名。
- 可写性、`set def` receiver、`ref` 绑定和 `&` 取地址都按展开后的显式路径判断。换句话说，`obj.name` 和 `obj.Inner.name` 的权限结果必须一致。
- 当前实现不支持 `set _ T`；V1 只支持 `_ T` 形式的嵌入字段。

## 4. call-like 初始化

```lona
var c = Complex(real = 1, img = 2)
var d = Complex(1, 2)
var e = Complex(1, img = 2)
```

说明：

- `Complex(...)` 是结构体类型名参与的 call-like 初始化，不是隐式创建出的同名构造函数实体。
- 语义阶段会把 `Type(...)` 解析成对应结构体的字段初始化路径，而不是普通顶层函数调用。
- 位置实参按字段声明顺序绑定；命名实参按字段名重排。
- 位置实参可以和命名实参混用，但位置实参必须全部写在前面。
- 缺字段、重复字段、未知字段、类型不匹配都会给出 targeted diagnostic。
- 普通顶层函数不能与结构体同名，因为 `Type(...)` 这类 call-like 初始化保留给结构体类型名。

## 5. 结构体中可以定义方法

```lona
struct Counter {
    set value i32

    def read() i32 {
        ret self.value
    }

    set def inc(step i32) i32 {
        self.value = self.value + step
        ret self.value
    }
}
```

说明：

- `set` 的本质不是“给结构体额外发明一套权限系统”，而是把结构体自身各个内存槽位的可写性显式写出来。
- 这样做的根因是：`const` 不能只停留在最外层 `=` 上，而必须沿着结构体字段访问继续传播；否则 `const Demo` 仍然可能通过 `obj.field = ...` 或 `obj.field.bump()` 改写自身内存。
- 因此，未标 `set` 的字段表示“这个槽位对结构体外部只读”；`set field` 才表示“这个槽位对结构体外部可写”。
- 对值字段，外部访问未标 `set` 的 `obj.field` 时，结果会按 `FieldType const` 看待，所以后续既不能直接赋值，也不能在其上调用需要可写接收者的方法。
- 对指针字段，未标 `set` 只冻结这个指针槽位本身，语义更接近 `P* const`，而不是 `Pointee const*`；也就是说它不自动承诺深层不可变。
- 当前方法接收者 `self` 仍然隐式按指针传递，但默认方法等价于隐藏的 `self Counter const*`。
- `set def` 方法的 hidden receiver 才是 `self Counter*`。因此普通 `def` 不能改写 `self`，根因不是“某个字段没标 `set`”，而是整个 receiver 已经是 `Self const*`。
- 即使某个字段本身写成 `set field`，普通 `def` 里也仍然不能写 `self.field = ...`；因为通过 `self` 看到的是整对象只读视图。
- 反过来，`set def` 拿到的是 `Self*`，所以它可以改写当前对象本身；这里字段是否标 `set` 不再限制结构体内部通过 writable receiver 修改自身。
- 指针上的 dot-like / call-like 自动解引用同样适用于 `self`，所以方法体里仍然直接写 `self.value`、`self.next()`。
- 方法调用语法不要求在接收者位置额外写 `&` 或 `ref`。
- 如果接收者本身是临时值，编译器会在调用点先物化一个临时槽位，再把它的地址作为隐藏 `self` 传入；因此 `Vec2(1, 2).normalize()` 和 `Vec2(1, 2).normalize_mut()` 这类写法都允许。

例如：

```lona
struct Holder {
    inner Counter
    link Counter*
    set current Counter
}
```

- 结构体外部的 `obj.inner` 按 `Counter const` 看待，因此 `obj.inner.inc(1)` 不允许。
- 结构体外部的 `obj.current` 保持可写投影，因此 `obj.current.inc(1)` 允许。
- 结构体外部的 `obj.link` 如果未标 `set`，只表示这个指针槽位本身只读；`obj.link = other` 不允许，但它不自动把 pointee 升级成 `Counter const*`。

## 6. 字段与方法可以混合出现

```lona
struct Buffer {
    data i32*
    size i32

    def empty() bool {
        ret self.size == 0
    }
}
```

## 7. 空结构体

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
- `struct Name` 与后面的 `{` 必须在同一行；如果 `struct Name` 已经单独成行，它就表示 opaque struct declaration，而不是下一行 `{ ... }` 的开头。

## 8. Opaque Struct

```lona
struct FILE
struct Handle
```

说明：

- bodyless `struct Name` 表示“只知道类型名存在，但不知道字段和布局”的 opaque struct。
- opaque struct 不能按值构造、不能按值做局部变量，也不能访问字段。
- 如果需要持有或传递它，写成 `FILE*`、`Handle*` 这类指针形式。
- 这条语义不只用于 C FFI；它本身就是语言里的明确类型边界。
