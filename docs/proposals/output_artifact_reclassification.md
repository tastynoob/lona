# Output Artifact Reclassification Plan

## Goal Description

当前 `lona-ir` 的输出模式混合了两种不同维度：

- 产物格式
  - LLVM IR
  - bitcode
  - native object
- 产物粒度
  - 单最终文件
  - 模块 bundle

现有 CLI 把这两件事压缩成了：

- `--emit ir`
- `--emit obj`
- `--emit objects`
- `--emit entry`

这导致两个现实问题：

1. `obj` / `objects` 的语义过于接近，但一个表示“单最终 object”，一个表示“模块 object bundle”，记忆成本高。
2. 未来如果继续推进增量编译、bitcode 持久化和 ThinLTO，当前命名很难稳定扩展。

本计划的目标是把输出模式重新按“模块级可复用产物”和“最终链接产物”分层，收成下面这组语义：

- `--emit bc`
  - 输出模块 bitcode bundle manifest
  - 每个模块都会得到可复用 `.bc`
  - 这是后续 `full/thin LTO` 的上游产物
- `--emit obj`
  - 输出模块 native object bundle manifest
  - 每个模块都会得到可复用 `.o`
  - 这是默认快路径和普通多 object 链接的产物
- `--emit linked-obj`
  - 输出单最终 object
  - 这是“所有模块先链接，再产出一个最终 `.o`”的路径
  - 也是 `--lto full` 的稳定输出目标
- `--emit ir`
  - 继续输出最终链接 LLVM IR
- `--emit entry`
  - 继续只输出 hosted entry object

同时：

- `--emit objects` 从稳定 CLI 中移除。
- 默认开发态继续优先走模块级 object bundle 快路径。
- LTO 的 source of truth 继续是模块 bitcode，而不是普通 native `.o`。

这里要明确一个实现边界：

- 普通 native `.o` 保留的是系统 linker 需要的符号、重定位和 section 信息。
- 它**不等于**“天然保留 LLVM IR 级跨模块优化信息”。
- 因此，未来如果要支持 `ThinLTO`，不能只靠“已经有 `.o`”这件事。
- 更合理的方向是：
  - 要么保留模块 bitcode + summary
  - 要么输出一种 LTO-capable object
  - 但无论哪种，`bitcode` 仍然是更基础的真相来源

所以这份计划不会把 `--emit obj` 定义成“已经实现 ThinLTO 的对象模式”。
它只会把 `obj` 定义成：

- 模块级 native object bundle
- 普通链接的默认快路径
- 未来可与 `--lto thin` 组合，但那会依赖额外 LTO-capable 产物约束，而不是当前这版 plain native object 本身

## Desired CLI Surface

目标 CLI 形态如下：

```bash
lona-ir input.lo
lona-ir --emit ir input.lo output.ll
lona-ir --emit bc input.lo output.manifest
lona-ir --emit obj input.lo output.manifest
lona-ir --emit linked-obj --lto full input.lo output.o
lona-ir --emit entry --target x86_64-unknown-linux-gnu hosted-entry.o
```

对应语义：

- 不带 `--emit`
  - 继续输出 AST JSON
- `--emit ir`
  - 输出最终链接 LLVM IR
- `--emit bc`
  - 输出模块 bitcode bundle manifest
- `--emit obj`
  - 输出模块 native object bundle manifest
- `--emit linked-obj`
  - 输出单最终 object
- `--emit entry`
  - 输出 hosted wrapper object

## LTO Matrix

建议把 LTO 语义和 artifact 语义明确分开。

### Stable in this plan

- `--emit ir --lto off|full`
- `--emit bc --lto off`
- `--emit obj --lto off`
- `--emit linked-obj --lto off|full`
- `--emit entry --lto off`

### Reserved for future

- `--emit bc --lto thin`
- `--emit obj --lto thin`
- `--emit linked-obj --lto thin`

这里的原则是：

- `full` 是 whole-program slow path
- `thin` 是未来链接策略
- `obj` / `bc` 是 artifact category

也就是说：

- 不应该把 `thin` 当成“某种 `.o` 文件天生具备的属性”
- 而应该把 `thin` 当成“链接阶段如何消费模块产物”的策略

## Manifest and Bundle Model

当前 `--emit objects` 的 manifest 只适合 object bundle。
重分类后更合理的方向是统一成 generic artifact bundle manifest。

建议格式升级成：

```text
format\tlona-artifact-bundle-v1
kind\tbc|obj
target\t<normalized-triple>
artifact\t<kind>\t<module-role>\t<absolute-path>
```

说明：

- `kind` 明确指出 bundle 里成员是 `.bc` 还是 `.o`
- `module-role` 继续保留 `root|dependency`
- 实际成员文件仍然写到 `<manifest>.d/` 或 `--cache-dir` 指定目录
- 每个成员仍可保留单独 metadata sidecar，继续承载：
  - source hash
  - interface hash
  - implementation hash
  - target triple
  - opt/debug profile
  - entry role
  - generic instance records

这意味着 bundle manifest 要表达“这是哪类 bundle”，而不是默认假设全是 object。

## Acceptance Criteria

Following TDD philosophy, each criterion includes positive and negative tests for deterministic verification.

- AC-1: CLI 输出模式重新按 artifact category 收口，`objects` 从稳定模式中移除。
  - Positive Tests (expected to PASS):
    - `lona-ir --emit bc input.lo out.manifest` 成功生成 bitcode bundle manifest。
    - `lona-ir --emit obj input.lo out.manifest` 成功生成 object bundle manifest。
    - `lona-ir --emit linked-obj input.lo out.o` 成功生成单最终 object。
    - `lona-ir --emit linked-obj --lto full input.lo out.o` 成功生成 full-LTO 最终 object。
  - Negative Tests (expected to FAIL):
    - `lona-ir --emit objects ...` 给出 targeted diagnostic，说明该模式已废弃并建议改用 `--emit obj`。
    - `lona-ir --emit entry input.lo out.o` 仍然拒绝把源码路径传给 `entry` 模式。
    - `lona-ir --emit obj --lto full ...` 不再被解释成“单最终 object”。

- AC-2: 内部 `OutputMode` 和 `WorkspaceBuilder` API 要按“模块 bundle / 最终单文件”重新命名，避免 `ObjectFile` / `ObjectBundle` 混义。
  - Positive Tests (expected to PASS):
    - `CompilerSession::runFile()` 能把 `bc`、`obj`、`linked-obj` 正确映射到不同 builder 路径。
    - `WorkspaceBuilder` 至少拥有清晰分离的：
      - `emitIR(...)`
      - `emitBitcodeBundle(...)`
      - `emitObjectBundle(...)`
      - `emitLinkedObject(...)`
    - `entry` 路径仍然不经过 source module load。
  - Negative Tests (expected to FAIL):
    - 不允许继续用旧的 `ObjectFile` 表示“最终单 object”，同时又让 `obj` 表示“模块 object bundle”。
    - 不允许让 `emitLinkedObject(...)` 继续复用旧 `emitObject(...)` 这个模糊命名。

- AC-3: `bc` 和 `obj` 都应成为模块级复用产物，bundle manifest 与缓存模型要兼容现有增量语义。
  - Positive Tests (expected to PASS):
    - `--emit bc` 和 `--emit obj` 都能按模块 key 写 bundle 成员。
    - `--cache-dir` 至少对 `bc` 和 `obj` 两种 bundle 模式都有效。
    - `ModuleArtifact` 继续能同时缓存 bitcode 与 object bytes，不要求二选一。
    - 当模块 body 改变但接口不变时，bundle 模式仍然只重编受影响模块。
  - Negative Tests (expected to FAIL):
    - 不能把 `bc` 设计成“只有最终 root module 一份 bitcode”。
    - 不能让 `obj` bundle 脱离 `ModuleArtifact` 复用逻辑，重新走一套独立缓存体系。

- AC-4: `lac` / `lac-native` 的默认快路径和 LTO 慢路径要与新 CLI 对齐。
  - Positive Tests (expected to PASS):
    - `lac` 默认调用 `lona-ir --emit obj` 收集模块 objects。
    - `lac-native` 默认调用 `lona-ir --emit obj` 收集模块 objects。
    - `lac --lto full` 和 `lac-native --lto full` 改为调用 `lona-ir --emit linked-obj --lto full`。
    - hosted `entry` object 仍然只在 system 路径需要时额外生成。
  - Negative Tests (expected to FAIL):
    - 不能保留 driver 层对 `--emit objects` 的硬编码。
    - 不能让 full-LTO 路径继续经过模块 object bundle 再回读 `.o`。

- AC-5: 文档与诊断要把“artifact category”和“LTO strategy”明确分开，避免误导用户。
  - Positive Tests (expected to PASS):
    - `commands.md`、`native_build.md`、`compiler_architecture.md`、CLI help 一致使用：
      - `bc` = 模块 bitcode bundle
      - `obj` = 模块 object bundle
      - `linked-obj` = 单最终 object
    - 文档明确说明：普通 `.o` 不是 ThinLTO 的充分条件。
    - 文档明确说明：`full` 当前稳定，`thin` 是后续扩展位。
  - Negative Tests (expected to FAIL):
    - 不能在 reference/runtime 文档里把 `obj` 直接描述成“保留 LLVM 级链接时优化信息的对象文件”。
    - 不能在未实现 `thin` 前给出看起来像已稳定支持的 CLI 示例。

## Path Boundaries

### Upper Bound (Maximum Scope)

这版计划允许做到：

- CLI 输出模式重命名
- `--emit bc` 新增
- `--emit objects` 移除或至少 deprecate
- `linked-obj` 取代当前“最终单 object”语义
- `lac` / `lac-native` 调整到新 artifact 分类
- 为 `ThinLTO` 预留正确的产物边界与命名

但不要求在这一版里完成：

- 真正的 ThinLTO 实现
- 跨进程 linked module 持久化缓存
- LTO-capable object 格式设计定稿
- 系统 linker plugin 集成

### Lower Bound (Minimum Scope)

最小可接受落地是：

- 新 CLI 语义收口成 `bc` / `obj` / `linked-obj`
- `objects` 不再作为主路径暴露
- `lac` / `lac-native` 不再依赖 `objects`
- 文档和诊断同步到新命名

如果实现阶段想分两步，允许先：

1. 引入 `bc` 和 `linked-obj`
2. 把 `obj` 改成模块 object bundle
3. 暂时保留 `objects` 作为兼容别名并输出 deprecation diagnostic

但最终稳定语义不应长期保留 `obj` / `objects` 两套等价模式。

### Allowed Choices

- Can use: 通用 bundle manifest，按 `kind=bc|obj` 区分成员类型。
- Can use: 在 `ModuleArtifact` 里继续同时缓存 bitcode 和 object bytes。
- Can use: `linked-obj` 继续基于当前 bitcode link 后再 codegen 的实现路线。
- Can use: 先在内部 `LTOMode` 里预留 `Thin`，但暂不开放到稳定 CLI。
- Can use: 兼容期内把 `--emit objects` 映射成 `--emit obj` 并给出 deprecation diagnostic。
- Cannot use: 把普通 native `.o` 直接宣传成“已经支持 ThinLTO”的稳定承诺。
- Cannot use: 让 `--emit obj` 同时表示“模块 object bundle”和“单最终 object”。
- Cannot use: 为 `bc` / `obj` 重新造一套与 `ModuleArtifact` 无关的缓存路径。

## Suggested Rollout

建议按下面顺序推进：

1. 先重命名 CLI 和内部 `OutputMode`
   - 新增 `bc`
   - 引入 `linked-obj`
   - `obj` 改成模块 object bundle
   - `objects` 进入兼容别名阶段
2. 再把 builder 接口和 manifest 格式泛化
   - `emitBitcodeBundle(...)`
   - `emitObjectBundle(...)`
   - `emitLinkedObject(...)`
3. 然后迁移 `lac` / `lac-native`
   - 默认快路径改用 `--emit obj`
   - full-LTO 改用 `--emit linked-obj --lto full`
4. 最后再考虑 `ThinLTO`
   - 明确是 bitcode+summary 方案，还是 LTO-capable object 方案
   - 再开放 `--lto thin`

## Related Paths

- [main.cc](/home/lurker/workspace/compiler/lona/src/main.cc) - 当前 `--emit` / `--lto` CLI 解析入口
- [session_types.hh](/home/lurker/workspace/compiler/lona/src/lona/driver/session_types.hh) - 当前 `OutputMode` / `LTOMode`
- [session.cc](/home/lurker/workspace/compiler/lona/src/lona/driver/session.cc) - `CompilerSession::runFile()` 输出分发
- [workspace_builder.cc](/home/lurker/workspace/compiler/lona/src/lona/workspace/workspace_builder.cc) - `emitObject()` / `emitObjectBundle()` / final bitcode link
- [module_artifact.hh](/home/lurker/workspace/compiler/lona/src/lona/module/module_artifact.hh) - 模块 bitcode / object artifact 缓存
- [commands.md](/home/lurker/workspace/compiler/lona/docs/reference/runtime/commands.md) - 当前 CLI 对外文档
- [native_build.md](/home/lurker/workspace/compiler/lona/docs/reference/runtime/native_build.md) - 当前 `lac` / `lac-native` 路径说明
- [compiler_architecture.md](/home/lurker/workspace/compiler/lona/docs/internals/compiler/compiler_architecture.md) - 当前 artifact / 增量 / LTO 架构描述
- [lac.sh](/home/lurker/workspace/compiler/lona/scripts/lac.sh) - hosted driver 当前对 `objects` / `obj --lto full` 的消费
- [lac-native.sh](/home/lurker/workspace/compiler/lona/scripts/lac-native.sh) - bare driver 当前对 `objects` / `obj --lto full` 的消费
