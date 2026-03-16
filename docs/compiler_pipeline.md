# 编译 Pipeline

当前 LLVM IR 编译链路分成两层：

- 单模块 compile pipeline
- 多模块调度与最终链接

## 入口

- session 创建时会初始化一个 `CompilePipeline`
- session 同时会初始化一个默认的串行 `ModuleExecutor`
- 默认 stage 定义在 [session.cc](/home/lurker/workspace/compiler/lona/src/lona/driver/session.cc)
- pipeline 基础结构在 [compile_pipeline.hh](/home/lurker/workspace/compiler/lona/src/lona/pass/compile_pipeline.hh)
- 模块执行器接口在 [module_executor.hh](/home/lurker/workspace/compiler/lona/src/lona/module/module_executor.hh)

## 当前默认阶段

1. `collect-declarations`
2. `lower-hir`
3. `emit-llvm`
4. `optimize-llvm`
5. `verify-llvm`
6. `print-llvm`

这些 stage 只负责“单个模块”的 IR 产物生成。

## 模块调度

- `CompilerSession` 先基于 import tree 构建 `ModuleBuildQueue`
- 默认由 `SerialModuleExecutor` 逐个执行模块编译任务
- 每个模块都会生成独立 `ModuleArtifact`
- 若 artifact 与当前源摘要、接口摘要和直接依赖接口摘要一致，则当前轮会直接复用，不重新 lowering/codegen

## 链接阶段

- 所有需要的模块 artifact 生成完后，session 会显式进入链接阶段
- 当前实现使用 LLVM linker 把 root 模块和其依赖模块的 IR artifact 拼装成最终 module
- `--emit-ir` 输出的是最终链接后的 LLVM IR，而不是 root 模块的半成品 IR

## 扩展约定

- 新的编译阶段优先作为 pipeline stage 注册，而不是继续往 `emitIR()` 里堆顺序代码。
- 需要计时的阶段，应当写入 `SessionStats`，这样 `--stats` 和 benchmark smoke 会自动反映变化。
- 如果某个阶段依赖前置分析结果，应把依赖体现在 stage 顺序里，而不是隐式依赖外部全局状态。
- 若后续引入线程池，应优先替换 `ModuleExecutor` 的实现，而不是重写模块状态和 artifact 管理。
