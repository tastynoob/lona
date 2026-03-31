# Next Plan

这份文档收纳已经从 reference 文档中抽离出来的后续规划。

它不是当前稳定语义说明。下面的内容都属于未实现或尚未收口的方向。

## 1. 指针语义的后续补完

当前 reference 文档没有把下面这些能力定义成稳定特性：

- 指针算术
- 完整的指针比较规则
- 更细的 pointer memory model

如果继续推进，应单独形成一份专门的 pointer memory model 文档，而不是继续塞回 reference 文档。

## 2. `ref` 的扩展位置

当前实现只支持：

- 局部 `ref` 绑定
- `ref` 参数

后续如果要扩展，候选位置包括：

- 结构体字段
- 数组元素类型
- 返回类型
- 全局变量

这类扩展需要先明确：

- 生命周期归属
- 拷贝语义
- ABI 表示

在这些点没有收口前，不应把它们写进 reference。

## 3. `T[]` 与动态视图

当前 `T[]` 仍然只是保留语法，不是稳定用户能力。

如果后续要引入动态数组 / 视图语义，更合适的方向是单独定义：

- slice
- view
- 或其它显式携带长度信息的运行时容器

而不是直接把 `T[]` 解释成半值半指针结构。

## 4. `Trait dyn` 的可写性扩展

当前稳定语义里，`Trait dyn` 仍然只支持 get-only 方法；只要 trait 中出现 `set def`，整个 trait 就不能进入动态分派。

如果继续推进，更合适的方向不是引入新的 dyn 关键字，而是沿用已有 `const` 规则，把 trait object 分成：

- `Trait dyn`
- `const Trait dyn`

并让 setter 只对可写 dyn receiver 放行。

这部分已经单独形成计划文档：

- [trait_v0_dyn_mutability.md](trait_v0_dyn_mutability.md)
