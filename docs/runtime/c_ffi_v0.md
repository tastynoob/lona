# C FFI v0

> 这是一份 `lona <-> C` 互操作 v0 的设计与现状说明。
> 当前仓库已经接通了 parser / sema / codegen 的最小稳定子集，包括：
>
> - top-level `#[extern "C"]` 函数导入 / 导出
> - `#[extern] struct` opaque type
> - `#[repr "C"] struct`
>
> 目标不是一步做到“完整 C 绑定系统”，而是先冻结一版最小、稳定、可实现的 FFI 契约，优先满足：
>
> - `lona` 调用现成 C 库
> - C 调用 `lona` 导出的普通函数
> - 后续继续扩展 callback、`#[repr "C"]` 聚合和更复杂链接模型

## 1. 状态与范围

`C FFI v0` 只定义：

- `native` 目标模式
- `Linux x86_64 SysV`
- `lona <-> C` 的函数调用边界
- 显式声明式 FFI，不做自动 header import

当前仓库已落地的 v0 子集：

- `#[extern "C"]` 顶层函数导入 / 导出
- `#[extern] struct` opaque 类型
- `#[repr "C"] struct`
- C-compatible 指针参数 / 返回值检查
- native ABI 与 C ABI 的 lowering 分流

`C FFI v0` 暂时**不**定义：

- `managed` 模式下的 C 互操作
- varargs
- `extern "stdcall"`、Windows ABI 或其它 calling convention
- union、bitfield、`long double`、vector / SIMD
- 自动解析 `.h` 头文件
- 宏、内联函数、预处理器桥接
- C++ ABI
- 动态装载 API 语义本身，例如 `dlopen` / `dlsym` 的高层封装

因此，这份文档讨论的是：

- `lona` 如何显式声明 C 边界
- 编译器如何对这些边界做类型检查和 lowering

而不是：

- “如何自动把任意 C 头文件直接导入为 `lona` 模块”

## 2. 设计原则

### 2.1 内部 ABI 与 C ABI 必须分离

当前仓库已经有一套 `lona -> lona` 的内部 ABI 草案，见 [native_abi_v0.md](native_abi_v0.md)。

`C FFI` 不应复用这套内部 ABI 规则，也不应让它们在实现层混成同一套分类器。

原因很直接：

- `native ABI` 允许语言语义驱动的特殊规则
  - 隐式 `self`
  - `T[*]`
  - `ref`
  - 小聚合直接返回
- `C ABI` 必须服从平台和 C 对象模型

因此推荐实现模型是：

- `NativeAbi`：只处理 `lona <-> lona`
- `CAbi`：只处理 `lona <-> C`

函数、函数指针和导出符号都需要显式携带 ABI kind。

### 2.2 v0 优先“可用”而不是“完整”

绝大多数第一批实用需求并不需要完整 C 语言覆盖，而是：

- libc 基本函数
- 系统调用封装库
- 以 opaque pointer 为核心的库接口
- 少量 `struct*` 风格 API

因此 v0 优先支持：

- 标量
- 裸指针
- opaque struct pointer
- `#[repr "C"] struct` pointer
- top-level `#[extern "C"]` 函数导入 / 导出

先不支持：

- varargs
- struct by-value
- callback
- header import

### 2.3 ABI 是函数类型的一部分

`#[extern "C"]` 不能只是“声明上的一个修饰词”。

从语义上，ABI 至少要进入：

- 顶层函数声明
- 顶层函数定义
- 函数类型
- 函数指针类型

当前实现已经把 ABI kind 放进 `FuncType`。
不过用户层仍未开放 callback / `#[extern "C"]` 函数指针语法；这部分还停留在实现预留，而不是稳定表面语法。

否则后续一旦引入：

- C callback
- `qsort` / `pthread_create` / 自定义回调

就会发现“函数签名相同但 ABI 不同”的情况无法正确表达。

### 2.4 C ABI 对象不携带 `lona native ABI` 版本字段

`#[extern "C"]` 对象的兼容性由目标平台的 C ABI 决定，而不是由 `lona native ABI` 决定。

因此：

- 纯 C ABI 对象不需要写入 `lona native ABI` 版本字段
- 只有含有 `lona -> lona` 原生调用边界的对象，才需要 native ABI 版本字段

这条规则的目标是避免把“`lona` 编译器生成的对象”与“使用 `lona native ABI` 的对象”混成一个概念。

### 2.4 C FFI 只接受 C-compatible 类型

不是所有 `lona` 类型都能直接过 C 边界。

v0 必须把“允许过 FFI 的类型集合”收死。

编译器应该在语义分析阶段直接拒绝：

- `tuple`
- `ref`
- 方法类型
- 不具备 C 布局保证的聚合

不要把错误留到最终链接甚至运行时。

## 3. 当前已实现的表面语法与语义边界

### 3.1 导入 C 函数

当前语法：

```lona
#[extern "C"]
def puts(msg u8 const[*]) i32

#[extern "C"]
def malloc(size u64) u8[*]

#[extern "C"]
def free(p u8[*])
```

语义：

- 有 `#[extern "C"]`
- 没有函数体
- 表示“引用外部 C 符号”

lowering 结果应是：

- LLVM external declaration
- 使用 C ABI
- 不做 `lona` 模块名 mangling

### 3.2 导出 `lona` 函数给 C 调用

当前语法：

```lona
#[extern "C"]
def lona_add(a i32, b i32) i32 {
    ret a + b
}
```

语义：

- 有 `#[extern "C"]`
- 有函数体
- 表示“定义一个可供 C 调用的符号”

lowering 结果应是：

- LLVM function definition
- 使用 C ABI
- 导出符号名默认为声明名本身

v0 暂不要求单独的 `export` 关键字。

### 3.3 opaque C struct

当前语法：

```lona
#[extern]
struct FILE

#[extern]
struct DIR
```

语义：

- 只声明存在这个 C 类型
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

当前语法：

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
- 允许作为 C 兼容结构体参与 FFI

v0 第一阶段建议只支持：

- `Point*`
- `ref` 风格改写成普通指针参数

先不支持：

- `Point` 按值传参
- `Point` 按值返回

### 3.5 callback 语法延后

callback 很重要，但它要求：

- ABI 进入函数类型
- 函数指针语法可表达 `#[extern "C"]`
- 语义分析能区分 native/internal 函数指针与 C 函数指针

因此建议：

- v0 第一阶段文法先不开放 callback 表面语法
- 但实现层要预留 `FuncType.abiKind`

等到 callback 阶段再定义函数指针的 `#[extern "C"]` 写法。

## 4. 类型边界

### 4.1 允许的标量

以下类型可直接过 C FFI：

- `u8` `i8`
- `u16` `i16`
- `u32` `i32`
- `u64` `i64`
- `f32` `f64`
- `bool`
- `void` 返回

约束：

- 整数宽度必须与目标 C ABI 宽度精确对应
- `bool` 在当前目标下映射到 C 的 `_Bool`
- 不引入与平台相关的 `long` / `unsigned long`

这意味着 v0 故意更偏向“固定宽度 C 互操作”，而不是“完整 C 语法镜像”。

### 4.2 指针

以下类型允许：

- `T*`
- `T[*]`
- `u8*` / `i8*`
- `u8[*]` / `i8[*]`
- `void*` 的语言映射，建议先用 `u8*` 或保留专门的 `opaque*` 设计到后续讨论
- 指向 opaque struct 的指针
- 指向 `#[repr "C"] struct` 的指针

以下类型禁止直接过 C FFI：

- `ref T`
- `(...: Ret)`，除非后续 callback 阶段明确带有 C ABI

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

- 字段类型必须本身 C-compatible
- 字段顺序按源码声明顺序固定
- 对齐 / padding 服从目标 C ABI

v0 暂不允许以下类型直接过 FFI：

- `tuple`
- 固定数组按值参数 / 返回
- 非 `#[repr "C"]` 的普通 `struct`

如果确实需要数组语义，建议在 v0 中改写成：

- 指针 + 长度

### 4.4 字符串

v0 不定义语言级字符串与 C 字符串自动桥接。

因此：

- 字节串字面量 / 只读 C 字符串入口优先统一写成 `u8 const[*]`
- `i8*` / `u8*` 仍然保留给真正按原始字节指针处理、或需要可写缓冲区的 API
- 编码、所有权、是否以 `0` 结尾，都由调用者自行保证

这虽然不够友好，但可以避免在 FFI v0 里提前引入字符串运行时设计。

## 5. ABI lowering 规则

### 5.1 总原则

所有标记为 `#[extern "C"]` 的函数都必须走单独的 `CAbi` lowering。

不允许：

- 把 `native ABI` 的小聚合直返规则直接沿用到 C 函数
- 把 `self` / `ref` / `T[*]` 的语言语义直接带到 C ABI

### 5.2 v0 的保守调用规则

v0 建议先把 C FFI 收成下面这组规则：

- 标量：直接传 / 直接返
- 指针：直接传 / 直接返
- opaque struct：只能以指针传
- `#[repr "C"] struct`：v0 第一阶段只允许以指针传

也就是说，第一阶段其实可以完全绕开：

- C 小 struct 按值返回
- C 小 struct 按值传参
- 平台 aggregate classification

这会让 `CAbi` 的第一版实现明显更简单。

### 5.3 `bool`

当前 `lona` 已经把 `bool` 的 ABI / 布局表示收成 1 byte。

对 C FFI 来说，v0 可以直接规定：

- `bool` 对应 `_Bool`
- ABI 上按平台规则传递

但需要补一条语义约束：

- `lona bool` 在导入 / 导出边界必须规范化为 `0 / 1`

### 5.4 名字和调用约定

`#[extern "C"]` 的效果至少包括：

- 不做 `lona` 模块名 mangling
- LLVM calling convention 使用 C convention
- ABI 分类器改走 `CAbi`

### 5.5 方法禁止直接导出为 C

v0 明确禁止：

- `#[extern "C"]` method

原因：

- method 带隐式 `self`
- 语义上更像带 receiver 的语言函数
- 直接暴露成 C 容易把语言调用约定泄漏到 FFI

如果用户需要把方法暴露给 C，应手写 top-level wrapper：

```lona
#[repr "C"]
struct Counter {
    value i32
}

#[extern "C"]
def counter_bump(self Counter*, step i32) i32 {
    self.value = self.value + step
    ret self.value
}
```

## 6. 符号与链接模型

### 6.1 符号名

v0 建议先采用最小规则：

- `#[extern "C"]` import：符号名默认等于声明名
- `#[extern "C"]` definition：导出符号名默认等于声明名

暂不引入：

- `link_name`
- `symbol_name`
- attribute 系统

如果后面确实需要绑定不同本地名 / 外部名，再单独补一套极小 attribute。

### 6.2 模块接口哈希

接口哈希后续必须把以下信息纳入签名：

- ABI kind
- 符号名
- opaque / `#[repr "C"]` 类型身份

否则会出现这种错误复用：

- 函数名字没变
- 参数类型表面也没变
- 但 ABI 从 `native` 切成了 `C`

### 6.3 链接输入

语言本身不负责描述所有链接器参数。

v0 更适合把下面这些能力放到 CLI / build system：

- `-lfoo`
- `-L/path`
- 额外 `.o`
- 额外 `.a`
- 额外 `.so`

也就是说：

- `lona` 源码只负责“声明需要的符号”
- “从哪里找到这些符号”由构建链路负责

## 7. 编译器落地建议

### 7.1 数据模型

建议至少引入：

- `enum class AbiKind { Native, C }`

并把它挂到：

- `AstFuncDecl`
- `ResolvedFunction`
- `FuncType`
- module interface signature

即使 callback 还没做，`FuncType` 也要带 ABI kind。

### 7.2 ABI 分类层

建议新增：

- `src/lona/abi/c_abi.hh`
- `src/lona/abi/c_abi.cc`

职责和 `NativeAbi` 平行：

- `classifyCType`
- `classifyCFunction`
- `emitCAbiFunctionType`
- `emitCAbiCall`
- `emitCAbiEntry`

不要把 `if (abi == C)` 的分支散落到 `collect` / `object` 各处。

### 7.3 类型检查

在语义分析阶段直接拒绝：

- `#[extern "C"]` method
- `#[extern "C"]` 中的 `ref`
- 非 C-compatible 的聚合
- opaque struct 按值使用

### 7.4 `#[repr "C"]` 布局

`#[repr "C"]` 的布局建议单独建一个显式分支，不要复用普通 `struct` 的“当前碰巧兼容”布局。

因为从语言设计上，`#[repr "C"]` 不是“注释”，而是：

- 布局承诺
- FFI 边界承诺
- 增量失效判断的一部分

## 8. 分阶段实施顺序

建议按下面顺序推进：

1. `#[extern "C"]` bodyless 顶层函数导入
2. `#[extern "C"]` 顶层函数导出
3. `#[extern] struct` opaque type
4. `#[repr "C"] struct` 与 `struct*` 传参
5. callback 与 C ABI 函数指针
6. 按值 struct 传参 / 返回
7. varargs
8. header import / bindgen

这个顺序的好处是：

- 每一步都可独立测试
- 每一步都能立即解锁真实库用例
- 不会一开始就掉进 C 全语言语义的坑里

## 9. 测试建议

v0 至少补这些回归：

1. C import
   - `malloc/free`
   - `fopen/fclose`
   - 非变参纯标量函数

2. C export
   - C harness 调用 `lona` 导出的 `#[extern "C"]` 函数

3. opaque type
   - `FILE*`
   - `DIR*`

4. `#[repr "C"] struct` 指针
   - C 侧写字段，`lona` 侧读字段
   - `lona` 侧写字段，C 侧读字段

5. 诊断
   - `#[extern "C"]` method 拒绝
   - `ref` 拒绝
   - 非 `#[repr "C"]` struct 拒绝
   - opaque struct 按值拒绝

6. 增量编译
   - ABI kind 变化
   - 符号名变化
   - `#[repr "C"]` 字段布局变化

## 10. 明确不进入 v0 的内容

以下内容不进入第一版：

- C++ ABI
- 自动 header import
- 宏导入
- varargs
- `setjmp` / `longjmp`
- signal handler 语义
- 按值 struct / union 全覆盖
- callback 以外的复杂函数指针推导
- 平台无关 FFI 抽象

## 11. 结论

`C FFI v0` 的核心收口应该是：

- `native ABI` 与 `C ABI` 分层
- `#[extern "C"]` 只先支持 top-level function
- 只支持 C-compatible 标量与指针
- opaque struct 和 `#[repr "C"]` struct 分开建模
- v0 第一阶段只做指针风格聚合互操作
- ABI kind 必须进入函数类型和接口哈希

先把这版做实，`lona` 就已经能开始使用大量现成 C 库，同时也为后续 callback、`#[repr "C"]` by-value 和自动绑定生成器打下稳定基础。
