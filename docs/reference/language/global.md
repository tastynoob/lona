# 全局变量

`global` 声明表示模块级存储，不再属于 root 模块的顶层执行语句。

## 1. 基本写法

```lona
global counter = 1
global total i64 = 42_i64
```

规则：

- `global name = expr` 走类型推断。
- `global name T = expr` 显式声明类型。
- `global` 绑定默认可变，不区分 `var` / `const` / `ref` 绑定形态。
- 非 `#[extern]` 的 `global` 当前必须带初始化器。

## 2. 导入与访问

本模块内直接按名字访问：

```lona
global counter = 1

def bump() i32 {
    counter = counter + 1
    ret counter
}
```

导入别的模块后，通过 `mod.name` 访问：

```lona
import dep

dep.counter = dep.counter + 1
ret dep.counter
```

## 3. extern 全局符号

```lona
#[extern]
global __lona_argc i32

#[extern]
global __lona_argv u8 const[*][*]
```

规则：

- `#[extern] global name T` 声明一个外部链接的全局符号。
- `#[extern]` 不接受参数；这里不需要 `extern "C"` 形式。
- extern global 必须显式写类型。
- extern global 不能带初始化器。

## 4. 当前初始化限制

这一版全局初始化只接通“静态字面量”子集：

- 数字、布尔、字符、字符串、`null`
- 数字字面量前的一层 `+` / `-`
- 显式类型检查仍然沿用普通初始化规则：整数族只能隐式转到整数族，浮点族只能隐式转到浮点族
- 字符串字面量只能初始化只读字节视图，例如 `u8 const[*]` 或 `u8 const*`

这意味着下面这种写法当前还不支持：

```lona
def seed() i32 {
    ret 1
}

global value = seed() // 当前不支持
```

如果你需要更复杂的初始化：

- 先声明一个简单静态初值
- 再在模块顶层语句或函数里完成运行时初始化
