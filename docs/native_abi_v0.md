# Native ABI v0

> 这是一份 `lona native` 路线的第一版 ABI 草案。
> 它的目标不是一步做到“高性能最优 ABI”，而是先冻结一版稳定、可实现、可测试的最小契约。

## 1. 状态与范围

`native ABI v0` 只定义：

- `Linux x86_64`
- `native` 目标模式
- `lona -> lona` 的内部调用边界
- freestanding / hosted 两条原生链路共享的语言 ABI

`native ABI v0` 暂时**不**定义：

- C FFI
- `extern "C"` 风格 ABI
- 其它平台
- varargs
- 异常展开
- vector / SIMD ABI
- 跨语言对象布局兼容

因此，这份文档描述的是：

- `lona` 语言语义如何 lowering 到 native 调用边界

而不是：

- “如何和 C 保持源级兼容”

## 2. 设计原则

### 2.1 先稳定，再优化

v0 优先保证：

- 规则简单
- 跨模块稳定
- 易于做增量失效判断
- 易于写测试和 debug

当前实现已经在这套主规则之上补了一层保守优化：

- 固定布局且总大小为 `1 / 2 / 4 / 8` byte 的聚合
- 可以走“单寄存器 SROA”直传 / 直返

但多寄存器拆分、平台特化寄存器分类，仍不进入 v0 主规则。

### 2.2 不自定义寄存器调用约定

`lona` 自己不重新发明一套寄存器分配规则。

v0 依赖：

- LLVM 默认 calling convention
- `Linux x86_64 SysV` 平台 ABI

`lona` 自己定义的是：

- 哪些语言类型在 ABI 边界按值传
- 哪些语言类型在 ABI 边界按指针传
- 聚合类型何时走间接返回
- `ref` / `T[*]` / 数组 / tuple 在 ABI 层如何表示

### 2.3 语言 ABI 与 FFI ABI 分离

v0 只冻结 `lona` 内部 ABI。

如果后续需要：

- 调 C
- 被 C 调
- 和其它语言共享数据布局

应额外定义 ABI adapter，而不是把 v0 直接等同于 C ABI。

## 3. ABI 类型分类

v0 把语言类型分成三类：

1. 标量类型
2. 指针类类型
3. 聚合类型

### 3.1 标量类型

下列类型属于标量：

- `u8` `i8`
- `u16` `i16`
- `u32` `i32`
- `u64` `i64`
- `f32` `f64`
- `bool`

ABI 规则：

- 参数按值传递
- 返回值按值返回

### 3.2 指针类类型

下列类型属于指针类：

- `T*`
- `T[*]`
- `ref T` 参数
- 函数指针

ABI 规则：

- ABI 边界统一表示为一个机器字大小的 `ptr`
- 参数按值传递这个地址值
- 返回值也可直接返回 `ptr`

注意：

- `T*`、`T[*]`、`ref T` 在语言语义上不同
- 但在 v0 ABI 表示上可以先相同

语义差异由前端 / HIR 保证，不由 ABI 区分。

### 3.3 聚合类型

下列类型属于聚合：

- `struct`
- `tuple`
- 固定数组 `T[N]`

ABI 规则：

- 默认不直接拆寄存器传递
- 统一走间接 ABI

当前实现补充了一条保守优化：

- 如果聚合有固定布局，且总大小正好是 `1 / 2 / 4 / 8` byte
- 则允许把整个聚合压成一个同宽整数寄存器传递

例如：

- `struct { i32, i32 }` 可以按 `i64` 传递
- `<i32, bool>` 在当前布局下也可以按 `i64` 传递

也就是说：

- 聚合返回：`sret`
- 聚合按值参数：ABI 上传 `ptr`

如果命中了上面的单寄存器 SROA 优化，则不走这条默认规则。

## 4. 数据布局规则

### 4.1 基本要求

v0 要求所有 native lowering 都使用同一份目标布局信息：

- target triple
- data layout
- 对齐规则

当前平台固定为：

- `Linux x86_64`

### 4.2 结构体

`struct` 采用：

- 按字段声明顺序布局
- 按字段自然对齐插入 padding
- 结构体整体按最大字段对齐

v0 不支持：

- packed struct
- 自定义字段对齐属性
- 显式 ABI 重排

### 4.3 tuple

`tuple` 的 ABI 布局等价于匿名 `struct`：

- `<T1, T2, T3>` 等价于按顺序布局的三字段聚合

因此：

- tuple 不拥有独立 ABI 特例
- 只复用聚合布局规则

### 4.4 固定数组

固定数组 `T[N]` 的 ABI 布局为：

- 元素连续排布
- 无额外头部
- 对齐由元素类型决定

多维数组按递归数组处理：

- `i32[4][5]` 等价于 “5 个 `i32[4]` 的连续布局”

### 4.5 指针类

下列类型在 ABI 上都先视为普通指针：

- `T*`
- `T[*]`
- `ref T` 参数
- `(...)* Ret`

它们的区别不在布局，而在语义约束：

- `T*`：原始指针
- `T[*]`：可索引指针
- `ref T`：非空别名参数

### 4.6 `bool`

v0 建议冻结：

- `bool` 的内存 / 聚合布局大小为 1 byte

即使内部 SSA 里继续使用 `i1`，落到：

- 栈槽
- 结构体字段
- tuple 元素
- 数组元素
- ABI 聚合布局

都应按 1-byte `bool` 处理。

这样可以避免后续对象布局与寄存器布尔表示混淆。

## 5. 函数 ABI 规则

### 5.1 标量参数

标量参数直接按值传递。

例如：

```lona
def add(a i32, b i32) i32
```

ABI 形态：

```text
i32 @add(i32, i32)
```

### 5.2 指针类参数

指针类参数直接按值传递地址。

例如：

```lona
def poke(p u8[*], i i32, v u8)
```

ABI 形态：

```text
void @poke(ptr, i32, i8)
```

### 5.3 `ref` 参数

`ref T` 参数在 ABI 上等价于：

- 一个指向 `T` 的指针参数

但语义要求：

- 调用方必须传可寻址对象
- callee 直接操作调用方对象
- 不做值拷贝

例如：

```lona
def inc(ref x i32) i32
```

ABI 形态：

```text
i32 @inc(ptr)
```

### 5.4 成员函数隐式参数顺序

成员函数在语义上带有一个隐式：

- `ref self`

因此，method lowering 到 ABI 时，需要先把隐藏参数顺序冻结下来。

v0 规定成员函数的参数顺序为：

1. `self`
2. `sret`，如果返回值是未命中单寄存器 SROA 的聚合
3. 显式形参，按源码声明顺序

也就是：

- `self > sret > normal`

其中：

- `self` 总是按引用接收
- ABI 上表现为一个 `ptr`
- `self` 不做值拷贝

例如：

```lona
struct Counter {
    value i32

    def bump(step i32) i32 {
        self.value = self.value + step
        ret self.value
    }
}
```

ABI 形态：

```text
i32 @Counter.bump(ptr, i32)
```

再例如成员函数返回聚合：

```lona
struct Pair {
    left i32
    right i32

    def swap() Pair {
        ret Pair(self.right, self.left)
    }
}
```

ABI 形态：

```text
i64 @Pair.swap(ptr)
```

这里 `Pair` 因为满足单寄存器 SROA 条件，所以直接按 `i64` 返回。

非成员函数不带 `self`，因此它们的隐藏参数顺序仍然只是：

1. `sret`，如果存在且返回值未命中单寄存器 SROA
2. 显式形参

### 5.5 聚合按值参数

聚合按值参数在 v0 中统一 lowering 成：

- ABI 上传入 `ptr`

当前实现允许一个更窄的优化分支：

- 固定布局
- 总大小为 `1 / 2 / 4 / 8` byte
- 压成一个同宽整数寄存器直接传参

语义上，这个参数仍然是“按值”：

- callee 入口需要把它复制到自己的本地槽
- 后续对形参的修改不能回写调用方对象

也就是说，v0 区分：

- 语言语义：按值
- ABI 表示：间接

例如：

```lona
def take(p Point) i32
```

ABI 形态：

```text
i32 @take(ptr)
```

如果 `Point` 的 ABI 大小满足单寄存器 SROA 条件，则会变成：

```text
i64 @take(i64)
```

### 5.6 聚合返回

聚合返回默认统一使用 `sret`。

也就是说：

- 调用方分配返回槽
- callee 接收一个隐藏的返回地址参数
- callee 把结果写入该槽
- LLVM IR 层通常把显式返回类型降成 `void`

当前实现允许一个更窄的优化分支：

- 固定布局
- 总大小为 `1 / 2 / 4 / 8` byte
- 压成一个同宽整数寄存器直接返回

例如：

```lona
def make() Point
```

ABI 形态：

```text
void @make(ptr sret(%Point))
```

这条规则适用于：

- `struct`
- `tuple`
- 固定数组

### 5.7 函数指针

函数指针类型的 ABI 形态等于其 lowering 后函数签名的地址值。

也就是说，函数指针在 v0 上没有额外封装头。

## 6. 入口 ABI

v0 还冻结一个最小 native 程序入口契约：

- `__lona_entry__() -> i32`

含义：

- freestanding 启动代码 `_start` 调用它
- 返回值作为进程退出码

当前 hosted 路线里的 `main` 只是对宿主工具链的适配入口，不视为 `lona` 语言 ABI 的核心定义。

## 7. 当前不纳入 v0 的优化

下列优化明确不进入 v0 主规则：

- 双寄存器或更多寄存器的聚合拆分传参
- 双寄存器或更多寄存器的聚合拆分返回
- `byval` / `inreg` / `sret` 之外的平台特化技巧
- 基于大小阈值的 ABI 分流

原因很简单：

- 这些策略会让跨模块 ABI 判断变复杂
- 会让“语言规则”和“性能启发式”混在一起

v0 先要求 ABI 规则是可预测的。

## 8. lowering 建议

建议单独引入一层 ABI 分类模块，例如：

- `src/lona/abi/native_abi.hh`
- `src/lona/abi/native_abi.cc`

职责只做三件事：

1. `classifyType(TypeClass*)`
2. `classifyFunction(FuncType*)`
3. `emitAbiFunctionType / emitAbiCall / emitAbiEntry`

这样可以避免：

- HIR lowering 到处散落 ABI 判断
- 后续 v1 优化时难以替换

## 9. 测试建议

v0 落地时至少补下列测试：

1. 类型布局测试
   - `size`
   - `align`
   - 结构体 padding
   - tuple / array 布局

2. 调用边界测试
   - 标量参数 / 返回
   - 指针类参数 / 返回
   - `ref` 参数
   - 聚合按值参数
   - 聚合 `sret` 返回
   - 单寄存器 SROA 聚合参数 / 返回

3. 跨模块 ABI 失效测试
   - 参数类型变化
   - 返回类型变化
   - `T*` / `T[*]` 变化
   - struct / tuple / array 布局变化

4. native 入口测试
   - `__lona_entry__`
   - hosted shim
   - freestanding `_start`

## 10. 结论

`native ABI v0` 的核心收口是：

- 标量直接
- 指针类直接
- 聚合默认间接
- 小固定布局聚合允许单寄存器 SROA
- 未命中 SROA 的聚合返回统一 `sret`
- 只支持 `Linux x86_64`
- 先做 `lona` 内部 ABI，不急着和 C ABI 混合

这版规则的目标不是性能极限，而是给 native 路线一个稳定、明确、可逐步优化的起点。
