# 函数声明示例

> 对应 `grammar.md` 的“3.3 结构体与函数声明”。

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
- 裸函数签名本身不是可传递的用户类型；如果要把函数当作值传入，必须写成显式函数指针。
- 在参数类型位置里，parser 只接受显式函数指针，例如 `(:)`、`(i32: i32)`、`(ref i32: i32)`。

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
var cb (ref i32: i32) = inc&<ref i32>
```

## 6. 函数体总是块语句

```lona
def abs(v i32) i32 {
    if v < 0 {
        ret -v
    }

    ret v
}
```

## 7. `#[extern "C"]` 顶层函数

```lona
#[extern "C"]
def puts(msg u8 const[*]) i32

#[extern "C"]
def lona_add(a i32, b i32) i32 {
    ret a + b
}
```

说明：

- `#[extern "C"]` 现在已经是 parser / sema / codegen 接通的稳定语法入口，不只是草案。
- 没有函数体时表示导入外部 C 符号；有函数体时表示导出一个 C ABI 函数。
- 当前只支持 `#[extern "C"]`，不支持其它 ABI 名称。
- FFI v0 只支持顶层函数；`#[extern "C"]` method 虽然可能被 parser 吃下，但会在语义阶段被拒绝。
- C callback 的用户层函数指针语法还没有开放；当前函数类型内部已经记录 ABI，但语言表面还不能写 `#[extern "C"]` 函数指针。
- 更细的边界，例如禁止 `ref` 参数、callback、普通聚合按值跨 C 边界，见 [../runtime/c_ffi.md](../runtime/c_ffi.md)。
