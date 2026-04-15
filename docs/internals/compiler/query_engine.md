# `lona-query` 查询前端

`lona-query` 的定位不是另写一套 LSP 专用 parser，而是复用当前 `lona` 编译器前端，提供一个更适合静态分析器和编辑器工具消费的查询入口。

外层如果要做真正的 LSP，建议把 `lona-query` 当作语义引擎，再额外包一层协议适配和文档版本管理。

## 1. 目标

当前设计目标：

- 复用 parser、模块图、`resolve`、`analysis`
- 提供可持续会话和 `reload` 语义
- 同时支持文本 REPL 和单行 JSON 输出
- 允许后续在外层包 TypeScript / LSP

当前不做的事情：

- 直接实现 LSP 协议
- 引入一套与编译器前端分离的独立语义前端
- 为了查询而重新实现 parser / name lookup / type analysis

## 2. 代码分层

当前 `src/tooling/` 主要分成三层：

- `main.cc`
  - 进程入口、命令行参数、REPL / stdin 驱动
- `command.*`
  - 命令解析、命令表注册、表驱动 dispatch
- `output.*`
  - 文本输出和 JSON 输出格式化
- `session.*`
  - 会话状态、项目重建、查询实现

这个分层的重点是把“命令解析”和“查询逻辑”拆开：

- 新增命令时，不需要继续在入口里堆 `if` 分支
- JSON 和文本输出共享同一组查询结果
- 外层如果以后要接 socket、RPC 或 LSP，也可以直接复用 `Session`

## 3. 复用的编译器前端

`lona-query` 当前直接复用：

- `WorkspaceLoader`
  - 构建 root paths、entry modules 和 transitive imports
- `CompilationUnit`
  - 持有源码、语法树、模块接口和类型缓存
- `resolve`
  - 名字绑定、局部变量绑定、顶层 executable 包装
- `analysis`
  - 表达式与局部变量的更多语义结果

会话内部当前长期持有这些状态：

- 当前活动 `CompilationUnit`
- 当前活动模块语法树
- `DiagnosticBag`
- `ResolvedModule`
- `HIRModule`
- analysis 构建所需的 `IRBuildState`

之所以把 `IRBuildState` 也一起保留，是因为 `analysis` 产物里仍然引用了它管理的类型和语义对象。

## 4. 当前查询边界

目前查询大致分成两类：

- 顶层与项目级查询
  - `info global`
  - `find`
  - 顶层 `pv` / `pt`
  - 导入模块成员的 `pt module.member`
- 局部和语义相关查询
  - `goto`
  - `info local`
  - 局部 `pv`
  - 对象成员的 `pv obj.member`
  - 多段 value path 的 `pv a.b.c.d`

现在的实现不是“纯 AST 查询”：

- root paths、entry modules 和模块关系来自 `WorkspaceLoader`
- 顶层声明主要来自当前模块接口和会话索引
- 局部变量可见性来自词法作用域
- 局部变量类型信息已经接上 `analysis`

当前还没有完全做成“所有查询都只查 `resolve` / `analysis` side table”。这是后续可以继续推进的方向。

## 5. Root 与 `reload`

当前会话以 root paths 和已加载 entry modules 为中心：

- `root <path...>`
  - 选择一组 root paths，并清空已加载 entry modules
- `open <module>`
  - 打开当前活动模块；如果模块未加载，则把它加入 entry 集合并增量加载依赖闭包
- `reload`
  - 重新加载当前已加载 entry modules 及其依赖图
- `reload <module>`
  - 使用 canonical 模块路径重新加载指定模块，并失效当前已加载图里依赖它的模块

这里的“只重载一个模块”并不意味着依赖它的模块完全不重算。当前更准确的语义是：

- 变更模块的源码和模块缓存会刷新
- 依赖它的模块语义状态会失效
- 最终仍然从当前已加载 entry 集合重建可查询状态

当前 `Session` 里需要区分两类路径：

- root 路径集合
  - 决定 canonical module namespace
- entry module 路径集合
  - 决定当前已经加载并会参与诊断的模块子图
- active module 路径
  - 决定 `ast`、`pv`、`pt`、`goto`、`info local` 这些查询当前落在哪个模块上
  - 也决定 `pt module.member` 能看到哪些导入模块，以及 `pv obj.member` 用哪个活动语义点解析对象类型

这一步已经足够支撑项目级静态分析和外层 LSP 原型，但还不是细粒度增量前端。

## 6. 为什么现在不单独做 LSP 前端

当前更偏向 `clangd` 的工程思路：

- parser / sema 继续复用编译器前端
- 外层再补持续会话、缓存和查询接口

这样做的好处是：

- 避免语言语义在“编译器”和“工具链前端”之间漂移
- 所有语义修复天然复用到 `lona-query`
- 可以先把模块级 `reload` 做稳，再逐步推进更细粒度的缓存

## 7. 后续方向

如果以后继续往 LSP 引擎方向推进，更合理的演化顺序是：

1. 继续补 `resolve` / `analysis` 可查询语义信息
2. 把更多查询从会话自建索引迁到编译器前端产物
3. 为顶层声明和函数体引入更细粒度的失效规则
4. 视需要再做函数体级或查询级增量

在那之前，`lona-query` 的现实目标仍然是：

- 有稳定命令
- 有稳定 JSON 包装
- 能重载项目
- 能为外层工具提供足够准确的语义查询
