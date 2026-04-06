# 函数声明示例

> 对应 `grammar.md` 的“3.3 结构体与函数声明”。
> 本文只讲顶层 `def`。结构体方法见 [struct.md](struct.md)，泛型函数与实例化见 [generic.md](generic.md)，C ABI 顶层函数见 [../runtime/c_ffi.md](../runtime/c_ffi.md)。

## 1. 无参数、无显式返回类型

```lona
def noop() {}
```

## 2. 无参数，直接写返回类型

```lona
def one() i32 {
    ret 1
}
```

## 3. 有参数、无显式返回类型

```lona
def printNumber(v i32) {
    ret
}
```

## 4. 有参数，并直接写返回类型

```lona
def add(a i32, b i32) i32 {
    ret a + b
}
```

## 5. 参数类型可以是复杂类型

```lona
def load(ptr i32*, size i32) i32 {
    ret *ptr
}

def apply(f (i32: i32), x i32) i32 {
    ret f(x)
}

def apply_ref(f (ref i32: i32), x i32) i32 {
    ret f(ref x)
}
```

说明：

- 当前函数参数和返回值默认按值处理；这条规则同样适用于结构体、tuple 和固定维数组。
- 如果需要共享并原地修改同一份对象，可以显式使用引用参数 `ref x T`，或显式传递指针类型 `T*`。
- 裸函数签名本身不是可传递的用户类型；如果要把函数当作值传入，必须写成显式函数指针，具体写法见 [type.md](type.md)。
- 在参数类型位置里，parser 只接受显式函数指针，例如 `(:)`、`(i32: i32)`、`(ref i32: i32)`。
- 参数列表当前不支持尾逗号；例如 `def add(a i32, b i32,) i32` 会在 parser 阶段报错。

例如：

```lona
def inc(ref v i32) {
    v = v + 1
}
```

调用引用参数时，调用点也必须显式写出 `ref`：

```lona
var x i32 = 1
inc(ref x)
```

如果使用命名参数，则写成 `inc(ref v = x)`。

函数指针或函数引用如果涉及 `ref` 参数，也必须把这点写进签名：

```lona
var cb (ref i32: i32) = @inc
```

## 5.1 顶层函数也可以声明泛型参数

```lona
def id[T](value T) T {
    ret value
}

def hash_one[T Hash](value T) i32 {
    ret value.hash()
}
```

说明：

- 泛型参数列表写在函数名后面的 `[...]`。
- generic v0 当前支持单 trait bound，例如 `[T Hash]`。
- 调用时既可以显式写成 `id[i32](1)`，也可以在可推断时直接写成 `id(1)`。
- 如果要把泛型函数当函数值使用，必须先实例化，例如 `@id[i32]`。
- 更完整的泛型规则见 [generic.md](generic.md)；trait bound 允许的能力边界见 [trait.md](trait.md)。

## 6. 函数体总是块语句

```lona
def abs(v i32) i32 {
    if v < 0 {
        ret -v
    }

    ret v
}
```

说明：

- `def name(...) Ret` 这一行如果已经以换行结束，就表示函数声明；它只声明签名，不提供函数体。
- 这种 bodyless `def` 可以用于模块接口或外部符号声明；如果当前编译单元里没有对应定义，最终是否能链接成功取决于链接阶段能否找到同名符号。
- 因此如果要写函数体，开块 `{` 必须和函数头写在同一行；`def add(a i32, b i32) i32` 下一行再写 `{` 当前不会被当成同一个函数体头。
