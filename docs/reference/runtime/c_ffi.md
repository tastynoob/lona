# C FFI v0

这份文档定义 `lona <-> C` 互操作的 v0 语法与语义边界。

v0 覆盖的稳定子集：

- top-level `#[extern "C"]` 函数导入
- top-level `#[extern "C"]` 函数导出
- `#[extern] struct` opaque 类型
- `#[repr "C"] struct`
- C-compatible 标量与指针的参数 / 返回值检查

v0 不覆盖：

- `managed` 模式下的 C 互操作
- varargs
- `extern "stdcall"`、Windows ABI 或其它 calling convention
- union、bitfield、`long double`、vector / SIMD
- 自动解析 `.h` 头文件
- 宏、内联函数、预处理器桥接
- C++ ABI
- `dlopen` / `dlsym` 之类动态装载 API 的高层封装
- callback 表面语法
- 聚合按值传参 / 返回

## 1. 范围

`C FFI v0` 只定义：

- `native` 目标模式
- `Linux x86_64 SysV`
- `lona <-> C` 的函数调用边界
- 显式声明式 FFI

也就是说，这份文档讨论的是：

- 如何显式声明 C 边界
- 哪些类型允许过 C 边界
- 编译器应如何理解这些边界

它不讨论：

- 自动把任意 C 头文件导入为 `lona` 模块
- 平台无关的通用 FFI 抽象

## 2. 核心规则

### 2.1 `native ABI` 与 `C ABI` 分离

`C FFI` 不复用语言内部 `native ABI` 规则。

原因是两者建模目标不同：

- `native ABI` 允许语言语义驱动的特殊规则
  - 结构体方法调用规则
  - `T[*]`
  - `ref`
  - 小聚合直接返回
- `C ABI` 必须服从平台和 C 对象模型

因此：

- `lona <-> lona` 调用走 `native ABI`
- `lona <-> C` 调用走 `C ABI`

### 2.2 ABI 是函数类型的一部分

`#[extern "C"]` 不是纯粹的声明修饰。

ABI 至少必须进入：

- 顶层函数声明
- 顶层函数定义
- 函数类型
- 函数指针类型

这条规则的目的是让“签名相同但 ABI 不同”的函数能被明确区分。

### 2.3 只允许 C-compatible 类型

不是所有 `lona` 类型都能直接过 C 边界。

v0 只接受一组显式收口的 C-compatible 类型。编译器应在语义分析阶段直接拒绝不合法类型，而不是把错误留到链接或运行时。

### 2.4 C ABI 对象不携带 `lona native ABI` 版本字段

`#[extern "C"]` 对象的兼容性由目标平台的 C ABI 决定，不由 `lona native ABI` 决定。

因此：

- 纯 C ABI 对象不需要 `lona native ABI` 版本字段
- 只有含有 `lona <-> lona` 原生调用边界的对象，才需要 native ABI 版本字段

## 3. 表面语法

### 3.1 导入 C 函数

```lona
#[extern "C"]
def puts(msg u8 const[*]) i32

#[extern "C"]
def malloc(size usize) u8*

#[extern "C"]
def free(p u8*)
```

语义：

- 带有 `#[extern "C"]`
- 没有函数体
- 引用外部 C 符号

效果：

- 使用 C ABI
- 不做 `lona` 模块名 mangling

### 3.2 导出 `lona` 函数给 C 调用

```lona
#[extern "C"]
def lona_add(a i32, b i32) i32 {
    ret a + b
}
```

语义：

- 带有 `#[extern "C"]`
- 有函数体
- 定义一个可供 C 调用的符号

效果：

- 使用 C ABI
- 导出符号名默认等于声明名

### 3.3 opaque C struct

```lona
#[extern]
struct FILE

#[extern]
struct DIR
```

语义：

- 只声明这个 C 类型存在
- 不暴露字段
- 只能以指针形式出现在 FFI 边界

例如：

```lona
#[extern]
struct FILE

#[extern "C"]
def fopen(path u8 const[*], mode u8 const[*]) FILE*

#[extern "C"]
def fclose(fp FILE*) i32
```

### 3.4 `#[repr "C"]` 结构体

```lona
#[repr "C"]
struct Point {
    x i32
    y i32
}
```

语义：

- 字段顺序固定
- 字段布局、对齐和 padding 服从目标 C ABI
- 可以作为 C 兼容结构体参与 FFI

v0 只保证下面这些写法：

- `Point*`
- `Point const[*]`
- 把需要 `ref` 的场景改写成普通指针参数

v0 不包含：

- `Point` 按值传参
- `Point` 按值返回

### 3.5 callback 不属于 v0

callback 语法要求 ABI 进入函数类型，并要求函数指针语法可表达 `#[extern "C"]`。

这部分不属于 v0 的稳定表面语法，因此：

- `#[extern "C"]` callback 不开放
- `#[extern "C"]` 函数指针不开放

## 4. 类型边界

### 4.1 允许的标量

以下类型可直接过 C FFI：

- `u8` `i8`
- `u16` `i16`
- `u32` `i32`
- `u64` `i64`
- `usize`
- `f32` `f64`
- `bool`
- `void` 返回

约束：

- 整数宽度必须与目标 C ABI 宽度精确对应
- `usize` 对应目标上的 `size_t`
- `bool` 对应 C 的 `_Bool`
- 不引入与平台相关的 `long` / `unsigned long`

### 4.2 指针

以下类型允许：

- `T*`
- `T[*]`
- `u8*` / `i8*`
- `u8[*]` / `i8[*]`
- 指向 opaque struct 的指针
- 指向 `#[repr "C"] struct` 的指针

以下类型禁止直接过 C FFI：

- `ref T`
- `(...: Ret)`
- 任何还没有显式 C ABI 语义的函数指针

### 4.3 聚合

v0 把聚合分成两类：

1. opaque C struct
2. `#[repr "C"] struct`

opaque C struct：

- 不能定义字段
- 不能按值构造
- 不能按值作为本地变量
- 只能出现在 `T*`

`#[repr "C"] struct`：

- 字段类型本身必须 C-compatible
- 字段顺序按源码声明顺序固定
- 对齐和 padding 服从目标 C ABI

以下类型不允许直接过 FFI：

- `tuple`
- 固定数组按值参数 / 返回
- 非 `#[repr "C"]` 的普通 `struct`

如果确实需要数组语义，v0 中统一改写成：

- 指针 + 长度

### 4.4 字符串

v0 不定义语言级字符串与 C 字符串的自动桥接。

因此：

- 只读 C 字符串入口优先写成 `u8 const[*]`
- `u8*` / `i8*` 保留给原始字节指针或可写缓冲区
- 编码、所有权和是否以 `0` 结尾，都由调用者自行保证

## 5. ABI 语义

### 5.1 基本规则

所有标记为 `#[extern "C"]` 的函数都必须走 `C ABI`。

不允许：

- 把 `native ABI` 的小聚合直返规则直接沿用到 C 函数
- 把结构体方法规则、`ref`、`T[*]` 的语言语义直接带到 C ABI

### 5.2 v0 的保守调用规则

v0 使用一组保守规则：

- 标量：直接传 / 直接返
- 指针：直接传 / 直接返
- opaque struct：只能以指针传
- `#[repr "C"] struct`：只允许以指针传

因此 v0 故意不覆盖：

- C 小 struct 按值返回
- C 小 struct 按值传参
- 平台 aggregate classification 细节

### 5.3 `bool`

`bool` 过 C 边界时遵循 `_Bool` 语义。

边界约束：

- 导入 / 导出时必须规范化成 `0 / 1`

### 5.4 名字与调用约定

`#[extern "C"]` 至少有三个效果：

- 不做 `lona` 模块名 mangling
- 使用 C calling convention
- ABI 分类改走 C ABI

### 5.5 method 不能直接导出为 C

v0 明确禁止：

- `#[extern "C"]` method

原因：

- method 属于语言内部的结构体语法
- C ABI 只接受显式 top-level 函数边界
- 需要导出时应改写成显式 wrapper

如果需要把方法暴露给 C，应手写 top-level wrapper：

```lona
#[repr "C"]
struct Counter {
    set value i32
}

#[extern "C"]
def counter_bump(p Counter*, step i32) i32 {
    (*p).value = (*p).value + step
    ret (*p).value
}
```

## 6. 符号与链接

### 6.1 符号名

v0 使用最小规则：

- `#[extern "C"]` import：符号名默认等于声明名
- `#[extern "C"]` definition：导出符号名默认等于声明名

不引入：

- `link_name`
- `symbol_name`
- 额外的导出属性系统

### 6.2 接口身份

接口身份至少应包含：

- ABI kind
- 符号名
- opaque / `#[repr "C"]` 类型身份

否则会把“表面签名相同但 ABI 不同”的接口误判成同一份接口。

### 6.3 链接输入

语言源码只负责声明需要的符号。

下面这些链接输入由 CLI 或构建系统负责：

- `-lfoo`
- `-L/path`
- 额外 `.o`
- 额外 `.a`
- 额外 `.so`

也就是说：

- `lona` 源码负责声明边界
- 构建链路负责提供符号实现

## 7. 不进入 v0 的内容

第一版不包含：

- callback
- 函数指针 ABI 推导
- 按值 struct / union 全覆盖
- varargs
- 自动 header import
- 宏导入
- C++ ABI
- `setjmp` / `longjmp`
- signal handler 语义

## 8. 小结

`C FFI v0` 的核心边界是：

- `native ABI` 与 `C ABI` 分层
- `#[extern "C"]` 只支持 top-level function
- 只支持 C-compatible 标量与指针
- opaque struct 和 `#[repr "C"] struct` 分开建模
- 聚合只走指针风格互操作
