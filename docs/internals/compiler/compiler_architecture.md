# 编译器架构总览

本文档描述当前 `lona` 编译器的整体架构，以及模块化、增量编译和 LLVM IR 生成在代码中的落点。

关于 `native / managed` 两种目标模式的边界定义，见 [../runtime/target_modes.md](../runtime/target_modes.md)。

## 1. 设计目标

当前架构围绕 4 个目标组织：

- 保持前端和后端分层，避免把编译逻辑继续堆回 `main.cc` 或单个大文件。
- 以 `.lo` 文件为基本模块单位，支持 `import` 驱动的多模块编译。
- 为增量编译保留稳定缓存点，包括模块接口缓存和已生成的模块 artifact。
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
- 负责把 CLI 输入映射到“输出 AST JSON / 最终 IR / 最终 object / object bundle”

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

Loader 解决的是“有哪些模块、它们怎么依赖、每个模块的语法树是什么”。

#### `WorkspaceBuilder`

职责：

- 基于 `ModuleGraph` 准备构建队列
- 判断模块 artifact 是否可复用
- 编译单个模块
- 保存模块级 bitcode / object artifact
- 在需要时把 root 模块和其依赖模块链接成最终 IR 或最终 object

Builder 解决的是“这些模块应该怎么编、哪些可以复用、最终如何得到一份完整产物”。

当前 `WorkspaceBuilder` 内部仍然持有一个默认的 `SerialModuleExecutor`。这意味着：

- 架构上已经为并行编译保留了执行器接口
- 但默认实现仍然按队列串行编译模块

### 2.4 Single-Module Frontend + LLVM Lowering

对应文件：

- `src/lona/pass/compile_pipeline.hh`
- `src/lona/pass/compile_pipeline.cc`
- `src/lona/resolve/resolve.hh`
- `src/lona/analyze/function.cc`
- `src/lona/analyze/module.cc`
- `src/lona/analyze/rules.cc`
- `src/lona/declare/support.cc`
- `src/lona/declare/function.cc`
- `src/lona/declare/struct.cc`
- `src/lona/declare/scanner.cc`
- `src/lona/declare/interface.cc`
- `src/lona/declare/global.cc`
- `src/lona/emit/debug.cc`
- `src/lona/emit/codegen.cc`
- `src/lona/visitor.hh`

单模块编译仍然是显式 pipeline：

1. `collect-declarations`
2. `define-globals`
3. `lower-hir`
4. `emit-llvm`
5. `optimize-llvm`
6. `verify-llvm`
7. `print-llvm`

其中：

- `collect-declarations` 收集当前模块和其直接 import 模块的声明，以及类型/函数/全局/trait/impl declaration 的可见接口
- `collect-declarations` 也会在接口层验证 trait 满足性、orphan rule 和 visible impl coherence
- `define-globals` 为当前模块的全局变量补齐 LLVM global storage 和静态 initializer
- `lower-hir` 执行 resolve 和 HIR 分析
- `lower-hir` 也负责把 `Trait.method(&value, ...)` / `Trait.method(ptr, ...)` / `value.Trait.method(...)` 绑定到 concrete trait impl method；同时也会把 `Trait dyn` / `h.method()` lower 到专用 trait-object HIR 节点
- `emit-llvm` 把函数级 HIR lowering 成 LLVM IR
- `emit-llvm` 也会为 `Trait dyn` 生成 witness table，并把动态调用降成间接 dispatch
- `optimize-llvm` 应用 LLVM 优化 pipeline
- `verify-llvm` 做 IR 验证
- `print-llvm` 只在显式文本 IR 输出路径上使用

`CompilePipeline` 只负责“单模块”的 lowering 和 codegen，不负责多模块调度和最终链接。

模块级入口的职责当前也已经分摊进这条单模块 pipeline：

- `resolve` / `analyze` 会为模块顶层执行体合成模块入口
- root 模块保留 `__lona_main__() -> i32` 这个特殊语言入口
- 非 root 模块会合成每模块唯一的内部 init entry
- `emit/codegen.cc` 会为模块入口补依赖初始化调用、一次性执行 guard 和结果缓存

按当前职责划分：

- `analyze/function.cc` 保留函数级表达式/语句分析主流程
- `analyze/module.cc` 负责模块级 HIR 聚合
- `analyze/rules.cc` 放函数分析共享规则，包括方法选择器、storage 约束和诊断辅助
- `declare/support.cc` 只保留声明阶段共享基础能力，例如顶层命名冲突、类型解析和通用声明辅助
- `declare/function.cc` 负责函数签名声明、extern C 约束和 LLVM function materialization
- `declare/struct.cc` 负责结构体声明、字段规则和成员布局收集
- `declare/scanner.cc` 负责结构体成员收集、类型扫描、单模块声明收集入口
- `declare/interface.cc` 负责模块接口收集与接口物化，包括 trait declaration、impl declaration、trait 满足性检查和 visible impl coherence
- `declare/global.cc` 负责全局变量定义与静态 initializer lowering
- `emit/debug.cc` 负责 LLVM debug info 构造
- `emit/codegen.cc` 负责函数级 LLVM lowering 与模块级 IR emission，包括 trait witness table 和 `Trait dyn` lowering

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
- 本地类型/函数/trait 绑定
- 类型解析缓存
- `interfaceHash` 和 `implementationHash`

当前阶段枚举：

- `Discovered`
- `Parsed`
- `DependenciesScanned`
- `InterfaceCollected`
- `Compiled`

`CompilationUnit` 与 `ModuleInterface` 一起构成当前模块级名字和接口边界；更细的“符号表分层”见
`docs/internals/compiler/symbol_table.md`。

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
- 保存结构体类型声明、trait declaration、impl declaration 和函数签名
- 保存派生类型，如指针、数组、函数类型和 `DynTraitType`

当前缓存的是“结构化语义接口”，不是简单字符串表，也不是 LLVM 对象。

它的作用是：

- 让多个 importer 在同一会话中复用同一个模块接口
- 避免重复扫描同一模块的接口定义
- 让 trait / impl declaration 变化能稳定进入 `interfaceHash`，从而打断 importer 复用

### 3.4 `ModuleCache`

文件：

- `src/lona/module/module_cache.hh`
- `src/lona/module/module_cache.cc`

职责：

- 以源文件路径为 key 维护 `ModuleInterface`
- 保证同一模块在同一会话中只保留一份接口缓存

对 trait v0 来说，这层还意味着：

- importer 看到的是结构化 trait 接口，而不是临时 AST 片段
- trait 方法签名与 visible impl declaration 的变化必须经过接口缓存传播
- same-session 增量复用要同时服从 `interfaceHash` 和模块 artifact 的 entry-role 区分

### 3.5 `ModuleArtifact`

文件：

- `src/lona/module/module_artifact.hh`
- `src/lona/module/module_artifact.cc`

职责：

- 保存某个模块已经生成的 bitcode / object artifact
- 记录该 artifact 对应的：
  - `sourceHash`
  - `interfaceHash`
  - `implementationHash`
  - 直接依赖的 `dependencyInterfaceHashes`
  - `targetTriple`
  - `optLevel`
  - `debugInfo`
  - `entryRole`
  - 是否包含 `lona native ABI` 边界

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
- 必须写成 canonical 模块路径，而不是 importer-relative 文件路径
- imported 模块的顶层成员通过 `file.xxx` 访问
- 模块搜索根是“root 文件所在目录 + 显式 `-I` roots”
- 所有搜索根共享一个扁平 canonical 模块命名空间；roots 不能重叠，也不能产生重复模块路径
- imported 模块也允许顶层可执行语句；这些语句通过模块初始化入口链执行

### 4.4 进入 Builder

`runFile()` 会根据 `--emit` 选择 Builder 输出模式：

- `builder.emitIR(...)`
- `builder.emitObject(...)`
- `builder.emitObjectBundle(...)`

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
3. 按需写入该模块自己的 bitcode / object bytes
4. 写入 `ModuleArtifact`
5. 存回 `workspace.moduleArtifacts`

当前每个模块 lowering 时都有自己独立的：

- `llvm::LLVMContext`
- `llvm::Module`
- `IRBuilder`
- `GlobalScope`
- `TypeTable`

也就是说，当前实现是“每模块一份 LLVM codegen 上下文”，而不是“整次会话共享一个 LLVMContext”。

模块级文本 LLVM IR 当前不会进入 `ModuleArtifact`。只有用户显式使用
`--emit ir` 时，Builder 才会在最终链接完成后把 linked module 打印成文本 IR。

### 4.7 最终链接

所有需要的模块 artifact 准备完成后，Builder 会：

- 从 root artifact 开始
- 按 `ModuleGraph.postOrderFrom(root)` 遍历依赖
- 将依赖模块的 bitcode artifact parse 成 LLVM module
- 用 LLVM linker 链接成最终 module

如果启用了 `--verify-ir`，会在最终链接后的 module 上再做一次验证。

### 4.8 可执行文件环境

当前仓库已经额外提供了两条本地可执行文件环境，见 `docs/reference/runtime/native_build.md`：

- system：复用宿主 ABI 和系统 CRT 启动对象
- bare：自带最小 `_start` 和 linker script

这层不在 `CompilerSession` 内部直接产出 ELF，而是走下面这条链路：

1. `lona-ir --emit ir --target <triple>` 生成目标相关的最终链接 LLVM IR
2. `lona-ir --emit bc --target <triple>` 或 `--emit obj --target <triple>` 生成模块 bundle
3. 显式 `lona-ir --emit linked-obj --lto full --target <triple>` 走 full-LTO 慢路径并生成最终对象文件
4. system 路径通过 `lac` 把 object bundle 或 full-LTO 最终 `.o` 交给系统 linker driver
5. bare 路径则用 `lac-native`、启动汇编和 linker script 产出 ELF

为了让两条链路都能稳定调用程序入口，当前实现把入口分成两层：

- 语言入口：`__lona_main__() -> i32`
- hosted system wrapper：`main(argc, argv) -> i32`

在这个稳定 ABI 之外，当前前端还会为 imported 模块按模块 key 合成内部 init entry 和配套的状态/result 全局：

- init entry 负责先调用依赖模块 init，再执行本模块顶层执行体
- 同一个模块 init 只会执行一次
- 非 0 返回值会沿 import 链向上传播回 root 的 `__lona_main__`

## 5. 当前增量编译语义

当前增量编译分两层：

- 同一 `CompilerSession` 内的内存态增量复用
- `--emit bc` / `--emit obj` 路径下基于 `cache-dir` 的磁盘 bundle 复用
- `--emit linked-obj` 路径下基于模块 bitcode 的磁盘中间缓存复用

已具备：

- 同一 `CompilerSession` 内，多次构建可复用 `ModuleInterface`
- 同一 `CompilerSession` 内，多次构建可复用 `ModuleArtifact`
- 同一 `CompilerSession` 内，多次构建可复用模块 object / bitcode artifact
- 多次独立 CLI 调用 `lona-ir --emit bc` 或 `--emit obj` 时，可通过同一个 `cache-dir/<manifest>.d/` 复用模块 bundle 成员
- 多次独立 CLI 调用 `lona-ir --emit linked-bc out.bc` 或 `--emit linked-obj out.o` 时，可默认通过 `./lona_cache/` 复用模块 bitcode；显式传 `--cache-dir <dir>` 时则改用该目录
- 当模块 body 改变但接口不变时，只重编该模块
- 当模块接口改变时，直接 importer 会失效并重新编译

当前还没有做的事：

- 跨进程 bitcode / linked-IR 持久化缓存
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
- 默认快路径仍然是多 object 链接；full-LTO 只在显式 `--lto full` 时启用
- 模块 artifact 当前缓存 bitcode 和可选 object；文本 LLVM IR 只在 `--emit ir` 时临时生成
- runtime profile 当前主要还是由 driver 约定，不是独立的一等配置对象；目前通过 `--target` 推导 hosted/bare 包装层
- `WorkspaceBuilder` 当前内部仍然包含较多细节，未来如果并行和持久化缓存继续增强，可能再拆出更明确的子组件

## 8. 后续演进建议

如果继续沿当前方向推进，建议优先按这个顺序演进：

1. 先把 `WorkspaceBuilder` 的输入输出整理成更显式的 build request / build result。
2. 再把当前“对象文件级持久化缓存”继续扩成“bitcode / linked artifact 的可选持久化缓存”。
3. 然后替换 `SerialModuleExecutor`，引入线程池并行编译。
4. 最后再考虑 ThinLTO 或更高性能的链接路径。
