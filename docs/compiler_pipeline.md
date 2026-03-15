# 编译 Pipeline

当前 LLVM IR 编译链路已经不再直接硬编码在 `CompilerSession::emitIR()` 里。

## 入口

- session 创建时会初始化一个 `CompilePipeline`
- 默认 stage 定义在 [session.cc](/home/lurker/workspace/compiler/lona/src/lona/driver/session.cc)
- pipeline 基础结构在 [compile_pipeline.hh](/home/lurker/workspace/compiler/lona/src/lona/pass/compile_pipeline.hh)

## 当前默认阶段

1. `collect-declarations`
2. `lower-hir`
3. `emit-llvm`
4. `optimize-llvm`
5. `verify-llvm`
6. `print-llvm`

## 扩展约定

- 新的编译阶段优先作为 pipeline stage 注册，而不是继续往 `emitIR()` 里堆顺序代码。
- 需要计时的阶段，应当写入 `SessionStats`，这样 `--stats` 和 benchmark smoke 会自动反映变化。
- 如果某个阶段依赖前置分析结果，应把依赖体现在 stage 顺序里，而不是隐式依赖外部全局状态。
