# ref 绑定与参数

> 本文只讲显式 `ref` 绑定与 `ref` 参数。显式指针见 [pointer.md](pointer.md)，结构体方法与 `set` 规则见 [struct.md](struct.md)。

## 1. `ref` 不是类型

`ref` 不是 `type-name` 的一部分，而是：

- 局部绑定语法
- 函数参数修饰符

默认赋值、传参与返回仍然按值处理。`ref` 只在显式写出时建立别名关系。

## 2. 局部 `ref` 绑定

局部引用写作：

```lona
ref a T = x
```

例如：

```lona
var x i32 = 1
ref a i32 = x
```

这里 `a` 是 `x` 的别名。

## 3. 绑定目标

`ref` 只能绑定到可寻址左值。

例如允许：

```lona
ref a i32 = x
ref b i32 = arr(0)
ref c i32 = point.x
ref d i32 = *ptr
```

例如不允许：

```lona
ref a i32 = 1
ref b i32 = x + y
ref c Point = Point(1, 2)
ref d i32 = foo()
```

这些右侧表达式都不是稳定、可别名的存储位置。

## 4. 读写语义

当 `ref` 出现在读取位置时，读取的是它绑定对象的当前值：

```lona
ref a i32 = x
var y = a
```

这里 `y` 拿到的是 `x` 当前的值拷贝。

当 `ref` 出现在赋值左侧时：

```lona
ref a i32 = x
a = 3
```

这里写入的是 `a` 绑定的对象，也就是 `x`。

## 5. 不可重绑定

```lona
ref a i32 = x
a = y
```

这不会把 `a` 改成引用 `y`。
它只表示把 `y` 当前的值写入 `a` 绑定的对象。

也就是说：

- `ref a T = x`：建立绑定
- `a = y`：写入绑定对象

## 6. `&ref` 的含义

对于引用：

```lona
ref a i32 = x
var p i32* = &a
```

`&a` 取得的是被绑定对象的地址。

这里 `&a` 等价于 `&x`。

## 7. `ref` 参数

`lona` 区分三种参数形式：

```lona
def by_value(x i32)
def by_pointer(p i32*)
def by_ref(ref x i32)
```

它们分别表示：

- `x i32`：按值传参
- `p i32*`：显式传地址值
- `ref x i32`：要求调用方显式传别名

例如：

```lona
def inc(ref x i32) {
    x = x + 1
}

def fill_first(ref row i32[4]) {
    row(0) = 1
}
```

调用时：

```lona
var x i32 = 0
var row i32[4] = {}

inc(ref x)
fill_first(ref row)
```

调用规则：

- `inc(ref x)` 合法，因为 `x` 是可寻址左值
- `inc(x)` 不合法，因为缺少显式 `ref`
- `inc(ref x + 1)` 不合法，因为 `x + 1` 不是可寻址左值
- `fill_first(ref row)` 合法，因为数组值本身有稳定存储

命名参数也必须显式写出 `ref`：

- `func(ref value)`
- `func(ref name = value)`

## 8. 与数组语义的关系

数组默认按值传参。

只有写成：

- `ref row i32[4]`
- `row i32[4]*`

函数体才会共享调用方对象。

因此不会出现 C 那种“数组参数看起来像值，实际自动变成指针”的情况。

## 9. 当前支持范围

当前实现只支持 `ref` 出现在：

- 局部变量声明
- 函数参数

当前实现不支持 `ref` 出现在：

- 结构体字段
- 数组元素类型
- 返回类型
- 全局变量

后续规划见 [../../proposals/next_plan.md](../../proposals/next_plan.md)。
