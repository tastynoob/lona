# 编译器架构总览

本文档描述当前 `lona` 编译器的整体架构，以及模块化、增量编译和 LLVM IR 生成在代码中的落点。

关于 `native / managed` 两种目标模式的边界定义，见 [../runtime/target_modes.md](../runtime/target_modes.md)。

## 1. 设计目标

当前架构围绕 4 个目标组织：

- 保持前端和后端分层，避免把编译逻辑继续堆回 `main.cc` 或单个大文件。
- 以 `.lo` 文件为基本模块单位，支持 `import` 驱动的多模块编译。
- 为增量编译保留稳定缓存点，包括模块接口缓存和已生成的 IR artifact。
- 为后续并行编译保留执行器接口，但当前默认仍是串行执行。

## 2. 当前核心分层

当前编译链可以理解为 4 层：

1. `Driver / Session`
2. `Workspace`
3. `Module Loading + Building`
4. `Single-Module Frontend + LLVM Lowering`

### 2.1 Driver / Session

对外入口是：

- `src/main.cc`
- `src/lona/driver/session.hh`
- `src/lona/driver/session.cc`
- `src/lona/driver/session_types.hh`

职责：

- 解析命令行参数
- 创建一次 `CompilerSession`
- 触发 `runFile(...)`
- 输出诊断
- 汇总并打印 `SessionStats`

`CompilerSession` 当前只保留很少的职责：

- 持有一个 `CompilerWorkspace`
- 持有一个 `WorkspaceLoader`
- 持有一个 `WorkspaceBuilder`
- 负责把 CLI 输入映射到“输出 AST JSON”或“输出 LLVM IR”

这层不再直接负责：

- import tree 构建
- artifact 复用判断
- 单模块 lowering / codegen
- 最终 IR 链接

这些动作都已经从 session 中剥离。

### 2.2 Workspace

核心文件：

- `src/lona/workspace/workspace.hh`
- `src/lona/workspace/workspace.cc`

`CompilerWorkspace` 是一次编译会话中的长期状态中心。它集中持有：

- `SourceManager`
- `DiagnosticEngine`
- `ModuleCache`
- `ModuleGraph`
- `ModuleBuildQueue`
- `moduleArtifacts`

可以把它理解为当前编译会话的内存态工作区。它不直接执行编译，而是提供稳定的共享状态。

### 2.3 Module Loading + Building

这层由两个对象组成：

- `WorkspaceLoader`
- `WorkspaceBuilder`

对应文件：

- `src/lona/workspace/workspace_loader.hh`
- `src/lona/workspace/workspace_loader.cc`
- `src/lona/workspace/workspace_builder.hh`
- `src/lona/workspace/workspace_builder.cc`

#### `WorkspaceLoader`

职责：

- 装载 root 模块
- 解析单个 `CompilationUnit`
- 遍历 `import`，构建 import tree
- 填充 `ModuleGraph`
- 校验 imported 文件是否只包含允许的顶层声明

Loader 解决的是“有哪些模块、它们怎么依赖、每个模块的语法树是什么”。

#### `WorkspaceBuilder`

职责：

- 基于 `ModuleGraph` 准备构建队列
- 判断模块 artifact 是否可复用
- 编译单个模块
- 保存模块级 LLVM IR artifact
- 将 root 模块和其依赖模块链接成最终 IR

Builder 解决的是“这些模块应该怎么编、哪些可以复用、最终如何得到一份完整 IR”。

当前 `WorkspaceBuilder` 内部仍然持有一个默认的 `SerialModuleExecutor`。这意味着：

- 架构上已经为并行编译保留了执行器接口
- 但默认实现仍然按队列串行编译模块

### 2.4 Single-Module Frontend + LLVM Lowering

对应文件：

- `src/lona/pass/compile_pipeline.hh`
- `src/lona/pass/compile_pipeline.cc`
- `src/lona/resolve/resolve.hh`
- `src/lona/sema/analysis.cc`
- `src/lona/sema/collect.cc`
- `src/lona/visitor.hh`

单模块编译仍然是显式 pipeline：

1. `collect-declarations`
2. `lower-hir`
3. `emit-llvm`
4. `optimize-llvm`
5. `verify-llvm`
6. `print-llvm`

其中：

- `collect-declarations` 收集当前模块和其直接 import 模块的声明
- `lower-hir` 执行 resolve 和 HIR 分析
- `emit-llvm` 把 HIR lowering 成 LLVM IR
- `optimize-llvm` 应用 LLVM 优化 pipeline
- `verify-llvm` 做 IR 验证
- `print-llvm` 生成模块级 artifact 文本

`CompilePipeline` 只负责“单模块”的 IR 生成，不负责多模块调度和最终链接。

## 3. 模块数据模型

模块系统目前围绕以下几个对象组织：

### 3.1 `CompilationUnit`

文件：

- `src/lona/module/compilation_unit.hh`
- `src/lona/module/compilation_unit.cc`

它表示一个 `.lo` 文件在本轮会话中的编译单元。

它持有：

- 文件路径、模块名、模块 key
- 源文件内容引用
- 语法树
- 模块阶段状态
- imported 模块别名表
- 本地类型/函数绑定
- 类型解析缓存
- `interfaceHash` 和 `implementationHash`

当前阶段枚举：

- `Discovered`
- `Parsed`
- `DependenciesScanned`
- `InterfaceCollected`
- `Compiled`

### 3.2 `ModuleGraph`

文件：

- `src/lona/module/module_graph.hh`
- `src/lona/module/module_graph.cc`

职责：

- 保存所有已装载模块
- 记录依赖边和反向依赖边
- 记录 root 模块
- 提供 post-order 遍历顺序

`ModuleGraph` 是当前 import tree 和构建顺序的基础。

### 3.3 `ModuleInterface`

文件：

- `src/lona/module/module_interface.hh`
- `src/lona/module/module_interface.cc`

职责：

- 缓存模块接口级语义信息
- 保存结构体类型声明和函数签名
- 保存派生类型，如指针、数组、函数类型

当前缓存的是“结构化语义接口”，不是简单字符串表，也不是 LLVM 对象。

它的作用是：

- 让多个 importer 在同一会话中复用同一个模块接口
- 避免重复扫描同一模块的接口定义

### 3.4 `ModuleCache`

文件：

- `src/lona/module/module_cache.hh`
- `src/lona/module/module_cache.cc`

职责：

- 以源文件路径为 key 维护 `ModuleInterface`
- 保证同一模块在同一会话中只保留一份接口缓存

### 3.5 `ModuleArtifact`

文件：

- `src/lona/module/module_artifact.hh`
- `src/lona/module/module_artifact.cc`

职责：

- 保存某个模块已经生成的 LLVM IR artifact
- 记录该 artifact 对应的：
  - `sourceHash`
  - `interfaceHash`
  - `implementationHash`
  - 直接依赖的 `dependencyInterfaceHashes`

这组摘要用于判断当前模块是否可以跳过重新编译。

## 4. 当前完整编译流程

从 `main` 到最终 LLVM IR 的链路如下：

### 4.1 创建 Session

`main.cc` 创建 `CompilerSession`，同时组装：

- `CompilerWorkspace`
- `WorkspaceLoader`
- `WorkspaceBuilder`

### 4.2 装载 root 模块

`runFile()` 调用 `loader.loadRootUnit(inputPath)`。

此时会：

- 通过 `SourceManager` 读取源文件
- 为源文件创建或复用一个 `CompilationUnit`
- 将其标记为 root

### 4.3 递归装载 import tree

`loader.loadTransitiveUnits(...)` 会从 root 出发：

- parse 每个模块
- 扫描顶层 `import`
- 解析 import 路径
- 把被导入模块加入 `ModuleGraph`

当前 `import` 的几个约束：

- 只能出现在顶层
- 写成无引号、无后缀的形式
- imported 模块的顶层成员通过 `file.xxx` 访问
- imported 文件不能包含顶层可执行语句

### 4.4 进入 Builder

如果输出模式是 LLVM IR，则 `runFile()` 调用：

- `builder.emitIR(rootUnit, options.compile, lastStats_, out)`

### 4.5 构建队列与增量复用

`WorkspaceBuilder` 会：

1. 根据 `ModuleGraph` 重建 `ModuleBuildQueue`
2. 使用 `ModuleExecutor` 逐个取出待编译模块
3. 对每个模块先检查现有 `ModuleArtifact` 是否可复用

artifact 可复用的条件是：

- `sourceHash` 一致
- `interfaceHash` 一致
- `implementationHash` 一致
- 所有直接依赖模块的 `interfaceHash` 一致

只要这些条件都满足，就跳过该模块的重新 lowering 和 codegen。

### 4.6 编译单个模块

当模块不能复用时，Builder 会：

1. 创建新的 `IRPipelineContext`
2. 运行单模块 `CompilePipeline`
3. 得到该模块自己的 LLVM IR 文本
4. 写入 `ModuleArtifact`
5. 存回 `workspace.moduleArtifacts`

当前每个模块 lowering 时都有自己独立的：

- `llvm::LLVMContext`
- `llvm::Module`
- `IRBuilder`
- `GlobalScope`
- `TypeTable`

也就是说，当前实现是“每模块一份 LLVM codegen 上下文”，而不是“整次会话共享一个 LLVMContext”。

### 4.7 最终链接

所有需要的模块 artifact 准备完成后，Builder 会：

- 从 root artifact 开始
- 按 `ModuleGraph.postOrderFrom(root)` 遍历依赖
- 将依赖模块的 IR artifact parse 成 LLVM module
- 用 LLVM linker 链接成最终 module

如果启用了 `--verify-ir`，会在最终链接后的 module 上再做一次验证。

### 4.8 可执行文件环境

当前仓库已经额外提供了两条本地可执行文件环境，见 `docs/runtime/native_build.md`：

- system：复用宿主 ABI 和系统 CRT 启动对象
- bare：自带最小 `_start` 和 linker script

这层不在 `CompilerSession` 内部直接产出 ELF，而是走下面这条链路：

1. `lona-ir --emit ir --target <triple>` 生成目标相关的最终链接 LLVM IR
2. `lona-ir --emit obj --target <triple>` 生成目标相关的对象文件
3. system 路径通过 `lac` 把 `.o` 交给系统 linker driver
4. bare 路径则用 `lac-native`、`llc-18`、启动汇编和 linker script 产出 ELF

为了让两条链路都能稳定调用程序入口，当前实现把入口分成两层：

- 语言入口：`__lona_main__() -> i32`
- hosted system wrapper：`main(argc, argv) -> i32`

## 5. 当前增量编译语义

当前增量编译是“单会话内的内存态增量复用”。

已具备：

- 同一 `CompilerSession` 内，多次构建可复用 `ModuleInterface`
- 同一 `CompilerSession` 内，多次构建可复用 `ModuleArtifact`
- 当模块 body 改变但接口不变时，只重编该模块
- 当模块接口改变时，直接 importer 会失效并重新编译

当前还没有做的事：

- 跨进程持久化缓存
- 文件系统级 artifact cache
- 更细粒度的函数级增量
- 真正的并行模块编译

## 6. 为什么当前架构比上一版更收敛

当前架构刻意收成了下面这组边界：

- `Session`：只管入口和结果
- `Workspace`：只管长期状态
- `Loader`：只管模块发现和 parse
- `Builder`：只管构建、复用、链接
- `CompilePipeline`：只管单模块 lowering / codegen

这样做的好处是：

- 概念数量受控，不会为了“看起来模块化”而拆出过多一跳包装层
- 每层的职责更稳定，后续更容易替换内部实现
- 并行编译和更强的增量能力都还有明确落点

## 7. 当前已知边界

当前实现仍然有几个明确边界：

- imported 模块当前只支持“直接 import 一层可见”，不做自动多层 re-export
- 模块 artifact 仍然是 LLVM IR 文本，不是 bitcode
- 最终链接通过“parse artifact IR -> LLVM linker”完成，尚未做更高性能的内存态链接
- runtime profile 当前主要还是由 driver 约定，不是独立的一等配置对象；目前通过 `--target` 推导 hosted/bare 包装层
- `WorkspaceBuilder` 当前内部仍然包含较多细节，未来如果并行和持久化缓存继续增强，可能再拆出更明确的子组件

## 8. 后续演进建议

如果继续沿当前方向推进，建议优先按这个顺序演进：

1. 先把 `WorkspaceBuilder` 的输入输出整理成更显式的 build request / build result。
2. 再把 artifact cache 从“会话内内存态”推进到“可选持久化缓存”。
3. 然后替换 `SerialModuleExecutor`，引入线程池并行编译。
4. 最后再考虑 link-time 优化、对象文件产物或更高性能的链接路径。
