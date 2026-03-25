# Module Object Build v0

## Goal

把 `lona` 早期“模块级 LLVM IR 缓存 + 根模块最终 IR 链接 + 单 `.o` 发射”的模式，
演进为更适合工程构建的两层模型：

- 默认构建单元改成“每个模块单独生成并缓存 `.o`”
- 根入口包装改成独立的 synthetic root object
- 最终可执行文件默认走“多 object 链接”
- 为后续链接后优化保留 bitcode / LTO 路线

这份计划同时覆盖两件必须做的事：

- 增量编译
- 链接后优化

## Status

当前这份计划的主要阶段已经完成：

- 默认快路径已经改成模块 object bundle
- root language entry / hosted entry 已拆成独立 shim object
- full-LTO 已收口到显式 `--lto full`
- `ModuleArtifact` 当前缓存的是 bitcode / object bytes，不再缓存文本 LLVM IR
- 文本 LLVM IR 只在显式 `--emit ir` 时生成

## Current State

当前实现的关键边界是：

- `ModuleArtifact` 缓存模块级 bitcode 和可选 object bytes
- `WorkspaceBuilder` 在需要最终 IR 或 full-LTO 最终 object 时，会把各模块 bitcode parse / link 成一个最终 `llvm::Module`
- `--emit ir` 输出的是最终链接后的 IR
- 默认 `--emit objects` 输出模块 object bundle
- `--emit obj` 当前保留为显式 full-LTO / helper 路径
- 显式 `--lto full` 路径当前已经会在最终链接后再跑一次优化；默认快路径仍然不会强制做全局优化

当前模型的优点是简单，但有三个现实问题：

1. 默认快路径和显式 LTO 路径都已存在，但 ThinLTO 仍未落地。
2. 当前 artifact 仍是会话内缓存，尚未做持久化。
3. `--emit obj` 作为 helper/inspect 路径还保留着，未来可以再继续收口。

## Design Principles

### 1. 先改内部构建单元，再决定外部 CLI 是否变化

对外接口可以暂时保持稳定，优先把内部 artifact 和链接模型改对。

也就是说，先把内部默认构建单元改成模块级 `.o`，再决定：

- `lona-ir --emit obj` 是否继续表示“单最终 object”
- 还是后续再单独引入 manifest / object-dir / archive 语义

### 2. 快路径和优化路径分离

默认增量构建应优先追求：

- 少重新编译模块
- 少重新发 object
- 直接链接 object

真正的链接后优化应作为单独路径设计，不要把“默认增量构建”和“全量 LTO”混成同一条慢路径。

### 3. 入口包装只存在于 root shim

语言入口和宿主入口都不应散落在普通模块 object 里。

推荐收口为：

- 普通模块 object：只包含该模块自身定义
- synthetic root object：负责 `__lona_main__`
- hosted root wrapper object：负责 `main(argc, argv)` 和 `__lona_argc` / `__lona_argv`

### 4. C ABI 和 Lona Native ABI 分离

- 纯 `extern "C"` object 不需要携带 `lona native ABI` 约束
- 含有 `lona -> lona` native ABI 边界的 object 必须继续写 ABI marker
- 后续多 object 链接时，ABI 兼容检查必须在链接前统一完成

## Target Model

### Fast Incremental Path

默认构建路径改成：

```text
source modules
  -> per-module AST / resolve / HIR / LLVM
  -> per-module optimized bitcode
  -> per-module object files
  -> synthetic root object
  -> linker
  -> executable
```

这条路径的关键点是：

- 模块级 `.o` 可以独立复用
- 根入口包装不再要求把所有模块重新拼成一个最终 LLVM module
- `lac` / `lac-native` 最终拿到的是多个 `.o`

### LTO Path

链接后优化路径单独设计为：

```text
source modules
  -> per-module optimized bitcode
  -> LTO / ThinLTO link
  -> final optimized object or executable
```

这条路径的关键点是：

- LTO 依赖 bitcode，而不是文本 LLVM IR 缓存
- 默认增量构建不强制走 LTO
- LTO 应当是显式开关，而不是默认副作用

## Artifact Model

`ModuleArtifact` 已经从“只存 IR”收口成“描述 bitcode / object / profile”的结构。

建议新增这些信息：

- object 产物路径
- 可选 bitcode 产物路径
- 当前 artifact 是否已经发过 object
- 当前 artifact 是否包含 `lona native ABI` 边界
- 当前 artifact 的 object format / target triple / opt level / debug info

建议保留当前已有的：

- source hash
- interface hash
- implementation hash
- direct dependency interface hash

其中缓存复用规则维持现有思路：

- 自身 source / implementation 未变时，模块 object 可直接复用
- 直接依赖 interface 未变时，不因依赖实现改动而重编当前模块

## Root Shim Model

根模块不再直接依赖“把所有模块 IR 合并后再补入口”的做法。

建议拆成三种 object：

1. module object
   - 普通模块 codegen 结果
2. language root object
   - 只负责建立 `__lona_main__`
3. hosted wrapper object
   - 只在 hosted target 上生成
   - 负责 `main(argc, argv)` 和参数全局

这样可以明确区分：

- 语言入口契约
- 宿主 ABI 适配层
- 模块自身 object

也能避免入口包装和模块 object 缓存互相污染。

## Link Model

### System Target

对 `x86_64-unknown-linux-gnu` 这类 hosted target，建议最终链路改成：

- 各模块 `.o`
- language root object
- hosted wrapper object
- 外部 C / Rust / 其它语言 `.o`
- 系统 linker driver

### Bare Target

对 `x86_64-none-elf` 这类 bare target，建议最终链路改成：

- 各模块 `.o`
- language root object
- bare startup object
- linker script
- `ld`

这样 bare 路线后续也可以摆脱“先发整份 IR 再喂 `llc`”。

## CLI Strategy

第一阶段建议尽量不打破现有用户心智：

- `--emit ir`
  继续表示“调试和检查用的 IR 输出”
- `--emit obj`
  短期内继续保留

但内部实现改成模块级 object 后，`--emit obj` 有两种可选落地方式：

1. 保持现语义
   - 继续输出单个最终 relocatable object
   - 由编译器或 linker 再把多个 module object 合成一个 `.o`
2. 只作为内部/调试语义
   - `lac` 直接消费多 object
   - 后续再给 `lona-ir` 单独引入 manifest / archive / object-dir 输出

建议先走方案 1，保持外部稳定，再视实际使用情况决定是否引入更显式的新输出模式。

## Implementation Phases

### Phase 0: 收口现有约束

目标：

- 明确当前“单最终 `.o` 并不等于链接后优化”
- 明确模块级 `.o` 是默认快路径，LTO 是显式慢路径

工作：

- 补这份计划文档
- 明确 `compiler_pipeline.md`、`native_build.md`、`system_crt_build_v0.md` 中的现状与后续方向

### Phase 1: 模块级 object artifact

目标：

- 每个模块完成 codegen 后可以直接得到自己的 `.o`
- 未改动模块可以直接复用已有 `.o`

工作：

1. 扩展 `ModuleArtifact`
   - 增加 object / bitcode 相关字段
2. 扩展 `WorkspaceBuilder`
   - 模块编译后直接发 object，而不只保存 IR 文本
3. 抽出 artifact 输出目录规则
   - target / opt / debug / module key 必须进入缓存键
4. 扩展 `SessionStats`
   - 新增 object 发射 / object 复用统计

验收：

- 同一 `CompilerSession` 内二次构建时，未变化模块不再重新发 `.o`
- `incremental_smoke` 能看到 object 复用

### Phase 2: synthetic root object

目标：

- 把 `__lona_main__`、hosted `main(argc, argv)` 从“最终 IR 链接阶段副作用”改成独立 root shim object

工作：

1. 单独生成 language root module
2. 单独生成 hosted wrapper module
3. bare / system 只在根链接时选择需要的 wrapper object
4. 普通模块 object 不再带入口包装逻辑

验收：

- 顶层可执行语句程序仍可正常构建
- hosted / bare 入口行为与当前语义保持一致
- root shim 缺失时能给出明确诊断

### Phase 3: 多 object 链接

目标：

- `lac` / `lac-native` 改为直接链接多 object
- 不再要求先把所有模块 IR 重新合并成一个最终 LLVM module

工作：

1. `WorkspaceBuilder` 暴露“root link inputs”列表
2. `lac`
   - 消费模块 object + root object + 外部 object
3. `lac-native`
   - 消费模块 object + root object + bare startup object
4. `lona native ABI` 兼容检查扩展到多 object

验收：

- hosted / bare smoke 都改为真实多 object 链接
- `lona` 与 C / Rust 生成的 `.o` 可以共同参与最终链接

### Phase 4: bitcode artifact 和链接后优化

目标：

- 引入真正的 post-link optimization，而不是依赖“最终单 `.o`”的表面形式

工作：

1. 每模块可选保存 optimized bitcode
2. 设计 LTO 开关
   - 例如 `--lto off|thin|full`
3. 为 root shim 也提供 bitcode 形式
4. 在 LTO 路径上重新做全局链接和优化

建议顺序：

- 先做 full LTO 原型
- 稳定后再考虑 ThinLTO

验收：

- LTO 路径能在多模块程序上工作
- 默认构建仍保持快路径，不因 LTO 退化为全量慢编译

### Phase 5: 清理旧 IR-only 链路

目标：

- 旧的“模块级 IR 文本缓存 + 最终 IR 全量重链”的默认路径不再承担正式构建职责

工作：

- 把它降级为 debug / inspect / tests 专用路径
- 继续保留 `--emit ir` 所需的 IR 输出能力
- 清理不再需要的中间接口

## Compatibility Rules

### Native ABI Marker

模块级 `.o` 成为正式构建单元后，需要把当前 `lona native ABI` marker 继续贯彻到每个 native object 上。

链接前检查规则建议保持：

- major version 必须完全一致
- minor version 允许向下兼容

对链接器/driver 来说，更实用的检查方式是：

- 收集所有 `lona native ABI` object 的版本
- 要求 major 全相同
- 最终链接目标选择一个不低于所有输入 minor 的 ABI 版本

纯 `extern "C"` object 不参与这个检查。

### External Object Interop

不需要按“语言来源”区分 object：

- `lona` object
- C object
- Rust object

最终只看：

- object format
- target triple / arch
- ABI
- 导出符号

真正需要区分的是：

- 含 `lona native ABI` 边界的 object
- 纯 C ABI object

## Risks

### 1. 缓存粒度变化会影响当前复用逻辑

当前复用已经扩成“模块 object / bitcode artifact 复用”；仍需要继续确认：

- interface 改动传播
- implementation 改动不外溢
- target/debug/opt/profile 隔离

### 2. root shim 可能重新引入入口重复定义问题

需要明确：

- hosted `main`
- `__lona_main__`
- 外部 `extern "C" def main(...)`

之间的优先级与冲突规则。

### 3. LTO 和默认增量构建容易互相拖累

必须避免：

- 为了支持 LTO，让默认构建也强制走 bitcode 全量链接

默认快路径和 LTO 慢路径要保持显式分离。

## Acceptance Criteria

满足以下条件后，才算这套计划完成：

1. 修改单个实现模块后，未受影响模块的 object 可直接复用。
2. `lac` 和 `lac-native` 默认都走多 object 链接。
3. 语言入口和宿主入口都只由 root shim object 负责。
4. `lona native ABI` 检查能覆盖多 object 链接。
5. 默认构建不强制开启 LTO。
6. 显式开启 LTO 时，确实存在链接后优化阶段。

## Immediate Next Step

实施时建议按下面顺序推进：

1. 先做 `ModuleArtifact` 扩展和模块级 object 发射。
2. 再做 root shim object。
3. 再改 `lac` / `lac-native` 到多 object 链接。
4. 最后再补 bitcode artifact 和 LTO。

这个顺序的原因是：

- 前三步先把“增量编译真正吃到 object 复用”的主收益落地
- 最后一段再补 LTO，避免一开始就把构建链复杂度拉满
