# 泛型 v0

> 本文汇总当前 `lona` 已接通的泛型语法与语义边界。
> 它对应 [grammar.md](grammar.md) 里的泛型参数、`Type[...]`、`name[T](...)`、`@name[T]` 等入口。
> trait bound 与 trait impl 的更完整语义见 [trait.md](trait.md)。

## 1. 当前支持的能力

generic v0 当前已经支持：

- 泛型结构体声明，例如 `struct Box[T]`
- 泛型顶层函数，例如 `def id[T](value T) T`
- 结构体方法自己的泛型参数，例如 `def map[U](...)`
- 类型位置的 applied generic type，例如 `Box[i32]`
- 调用位置的显式实例化，例如 `id[i32](1)`
- 调用位置的类型实参推断，例如 `id(1)`
- 泛型函数实体引用，例如 `@id[i32]`
- 单 trait bound，例如 `[T Hash]`
- generic struct declaration 上的单 trait bound，例如 `struct Box[T Hash]`
- generic impl body，例如 `impl[T Hash] Hash for Box[T] { ... }`

不在这版稳定范围里的内容见本文最后一节。

## 2. 泛型参数声明

### 2.1 泛型结构体

```lona
struct Box[T] {
    value T
}

struct Pair[A, B] {
    left A
    right B
}
```

规则：

- 泛型参数列表统一写在名字后面的 `[...]`。
- `struct Box[T]` 表示 `Box` 是一个类型模板，不是可直接作为 runtime concrete type 使用的完整类型。
- 类型参数在该泛型项自己的声明体内可见，包括字段类型、方法签名和方法体里的局部类型位置。

### 2.2 泛型顶层函数

```lona
def id[T](value T) T {
    ret value
}

def keep_ptr[T](ptr T*) T* {
    ret ptr
}
```

规则：

- 顶层 `def` 可以声明自己的泛型参数。
- 类型参数在参数类型、返回类型和函数体内部可见。
- 如果泛型函数没有被实例化，就不会生成具体 runtime 函数实体。

### 2.3 结构体方法自己的泛型参数

```lona
struct Box[T] {
    value T

    def echo[U](value U) U {
        ret value
    }
}
```

规则：

- 结构体方法可以在结构体自己的类型参数之外，再声明一组自己的类型参数。
- 方法体里同时能看到外层结构体参数和方法自己的参数。
- 结构体方法泛型实例化同样支持显式 `[...]` 和按参数类型推断。

## 3. 类型位置的泛型实例化

```lona
var box Box[i32]
var pair Pair[i32, bool]
var imported dep.Box[Point]
```

规则：

- 类型位置统一使用 `Type[...]`。
- `Box[i32]`、`Pair[i32, bool]`、`dep.Box[Point]` 都是 applied generic type。
- bare template 不是 concrete type，因此 `var box Box`、`var ptr Box*` 都会被拒绝。
- `Name<T>` 这种 angle-bracket 写法在 generic v0 里不是合法类型语法，应改写成 `Name[T]`。

常见限制：

- `Type[...]` 只能应用到真正的泛型类型；对非泛型类型写 `Box[i32]` 会报错。
- 类型实参数量必须和声明时的类型参数数量一致。
- 目前只支持类型实参，不支持值级模板实参。

## 4. 调用位置的泛型实例化与推断

### 4.1 显式类型实参

```lona
def id[T](value T) T {
    ret value
}

def main() i32 {
    ret id[i32](1)
}
```

规则：

- 泛型函数调用可以显式写成 `name[T](...)`。
- imported generic function 也一样，例如 `dep.id[i32](1)`。
- 如果显式写了 `[...]`，类型实参数量必须匹配泛型参数列表。

### 4.2 按实参类型推断

```lona
def id[T](value T) T {
    ret value
}

def main() i32 {
    ret id(1)
}
```

规则：

- 当前编译器会从调用实参的已知类型里推断泛型参数。
- 推断支持嵌套在指针、`const`、tuple 和 applied generic type 里的类型参数。
- 如果某个类型参数完全无法从调用点推断出来，必须显式写 `[...]`。

例如：

```lona
def choose[T](value i32) T {
    ret value
}
```

这里 `choose(1)` 无法推断 `T`，需要改成 `choose[i32](1)` 这类显式调用。

## 5. 函数实体引用与泛型函数

```lona
def id[T](value T) T {
    ret value
}

var cb (i32: i32) = @id[i32]
ret cb(1)
```

规则：

- 泛型函数不能直接作为 runtime value 使用。
- 如果要取得函数实体引用，必须先实例化，例如 `@id[i32]`。
- `@id` 这种未实例化的泛型函数引用会被拒绝。
- `@id[i32](1)` 等价于 `(@id[i32])(1)`。

## 6. 单 trait bound

### 6.1 泛型函数上的 bound

```lona
trait Hash {
    def hash() i32
}

def hash_one[T Hash](value T) i32 {
    ret value.hash()
}
```

规则：

- generic v0 每个类型参数只支持一个 trait bound。
- `T Hash` 表示 `T` 在实例化点必须满足 trait `Hash`。
- bound satisfaction 在实例化点检查，不在模板声明点提前假设成立。

### 6.2 泛型结构体上的 bound

```lona
struct Box[T Hash] {
    value T

}

impl[T Hash] Hash for Box[T] {
    def hash() i32 {
        ret Hash.hash(&self.value)
    }
}
```

规则：

- 结构体声明本身也可以带单 bound。
- `Box[Point]` 这类具体 applied type 被物化时，会检查 `Point` 是否满足 `Hash`。
- generic impl body 也使用同一套参数列表和同一套 bound 规则。

## 7. unconstrained 与 bounded 参数能做什么

### 7.1 unconstrained 参数

```lona
def keep_ptr[T](ptr T*) T* {
    ret ptr
}

def size_of_item[T]() usize {
    ret sizeof[T]()
}
```

unconstrained `T` 当前只稳定支持：

- 出现在类型位置
- 参与签名替换
- 参与 `sizeof[T]()`
- 在局部变量类型、指针类型、tuple 类型、applied generic type 里继续被引用

当前不会为 unconstrained `T` 自动开放这些能力：

- `obj.hash()` 这类成员方法调用
- `obj.value` 这类字段读取
- `left == right`、`left + right` 这类运算符
- “先通过 helper 返回值绕一下”再访问成员

### 7.2 bounded 参数

```lona
trait Hash {
    def hash() i32
}

def hash_one[T Hash](value T) i32 {
    ret value.hash()
}
```

bounded `T` 当前会额外开放：

- bound trait 提供的方法直接调用，例如 `value.hash()`
- 显式 trait-qualified static call，例如 `Hash.hash(&value)`

但当前仍然不会开放：

- 裸字段访问，例如 `value.field`
- 裸字段写入
- 仅凭 bound 自动获得运算符能力
- `value.Trait.method(...)` 这种 receiver-trait 路径

## 8. 常见诊断与限制

generic v0 当前有这些明确边界：

- bare generic template 不能出现在 runtime type 位置，例如 `var p Box*`
- `Name<T>` 不合法，类型应用统一写 `Name[T]`
- 泛型函数不能先当 runtime value 再等后面推断；需要直接调用，或者先写成 `@name[T]`
- trait 方法自身不能声明 generic parameter
- 每个类型参数只支持一个 trait bound
- multi-bound 例如 `[T Hash + Eq]` 还不支持

## 9. 当前明确不支持

- multi-bound generic constraints
- generic trait
- trait method 自己再带 generic parameter
- 值级模板参数
- 默认模板实参
- specialization / partial specialization

如果需要具体的 trait-bound 运行语义、`impl[T Trait] Trait for Box[T]` 的约束，继续看 [trait.md](trait.md)。
