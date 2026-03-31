# Trait v0 Dyn Mutability Plan

## Goal Description

基于当前已经落地的 trait v0，实现下一版 `Trait dyn` 可写性扩展，使 trait 中的 `set def` 不再导致整个 trait 被排除在动态分派之外，而是改为：

- `Hash dyn`
  - 表示可写动态 trait object
  - 可以调用 `def`
  - 也可以调用 `set def`
- `const Hash dyn`
  - 表示只读动态 trait object
  - 只能调用 `def`
  - 不能调用 `set def`

这版计划沿用现有 `const` 规则，不引入新的 `readonly dyn` / `mut dyn` 语法，也不改变现有：

- `trait`
- `impl Type: Trait`
- `Trait.method(value, ...)`
- `cast[Trait dyn](&value)`

的基本形态。

其中 `cast[Trait dyn](&value)` 只表达“构造 trait object”这件事；
如果借用源是 `const`，那么结果会自动继承为只读 trait object。
也就是说：

- `&value` 是可写借用时，结果是可写的 `Trait dyn`
- `&const_value` 是只读借用时，结果是只读的 `const Trait dyn`

这条规则不要求用户显式写出 `cast[const Trait dyn](...)`。

更本质地说，`cast[Trait dyn](&value)` 的结果仍然是当前那种：

- `data_ptr`
- `witness_ptr`

二元组语义。

其中决定 getter / setter 可调用边界的不是 `witness_ptr`，而是 `data_ptr`
在类型擦除后仍然保留的 constness：

- `T* -> any*`
- `T const* -> any const*`

因此：

- getter 可以接受 `any*` 或 `any const*`
- setter 必须拿到可写的 `any*`

这也是为什么 `cast[Hash dyn](&const_value)` 可以成功构造 trait object，
但后续 setter 调用仍然必须被拒绝。

本计划的核心目标是把“trait 级别的 dyn-compatible 一刀切”改成“method + receiver mutability 级别的动态调用规则”，同时尽量复用当前：

- `DynTraitType`
- `{data_ptr, witness_ptr}` trait object 表示
- witness table slot 顺序
- static trait dispatch 语义

而不扩大到 owning trait object、泛型 trait 或 default method。

## Acceptance Criteria

Following TDD philosophy, each criterion includes positive and negative tests for deterministic verification.

- AC-1: `Trait dyn` 的可写性语义与 `const` 规则打通，trait object 的只读/可写差异在类型层可见。
  - Positive Tests (expected to PASS):
    - 从非 `const` concrete object 构造 `Hash dyn` 成功。
    - 从非 `const` concrete object 构造 `const Hash dyn` 成功。
    - `cast[Hash dyn](&const_value)` 可以成功，且结果自动继承为只读 trait object。
    - `Hash dyn` 可以作为局部变量、参数、返回值和指针目标类型稳定工作。
  - Negative Tests (expected to FAIL):
    - `var h Hash dyn = cast[Hash dyn](&const_value)` 会因为试图接收成可写 trait object 而被拒绝。
    - 任何路径都不能通过 `cast[Hash dyn](...)` 偷偷去掉 source 的 `const` 资格。
    - 不能通过赋值或隐式转换把 `const Hash dyn` 升格成 `Hash dyn`。

- AC-2: 带 `set def` 的 trait 可以构造动态 trait object，但 setter 调用只对可写 dyn receiver 放行。
  - Positive Tests (expected to PASS):
    - `trait CounterLike { def read() i32; set def bump(step i32) i32 }` 可以用于构造 `Hash dyn` / `const Hash dyn` 风格的 trait object。
    - `const CounterLike dyn` 可以成功调用 `read()`。
    - `CounterLike dyn` 可以成功调用 `read()` 和 `bump(...)`。
    - `set def make_view() CounterLike dyn { ret cast[CounterLike dyn](&self) }` 这类 writable self forwarding 能通过并运行。
  - Negative Tests (expected to FAIL):
    - `const CounterLike dyn` 调用 `bump(...)` 会报 targeted diagnostic。
    - `const CounterLike dyn*` 通过 implicit deref 调用 `bump(...)` 也会被拒绝。
    - 仍然不允许通过临时值或非可寻址 source 构造可写 trait object。
    - `cast[CounterLike dyn](&const_value)` 虽然能成功构造 trait object，但不能借此绕过 setter 对 writable `data_ptr` 的要求。

- AC-3: 动态调用解析从“整个 trait 是否 dyn-compatible”改为“当前方法是否满足 receiver mutability 约束”，且不影响静态路径。
  - Positive Tests (expected to PASS):
    - `Trait.method(value, ...)` 继续支持 `set def`，不经过 witness table。
    - `h.read()` 在 `Hash dyn` 和 `const Hash dyn` 上都能成功解析。
    - `h.bump()` 只会在 `Hash dyn` 上解析为合法 trait-object call。
    - imported trait 和 imported impl 也遵守同样的 getter/setter 动态调用规则。
  - Negative Tests (expected to FAIL):
    - 不能再因为 trait 中出现单个 `set def` 就整体拒绝 `Trait dyn` 构造。
    - 普通 `obj.method(...)` lookup 不会因为这一版计划而自动注入 trait method。
    - setter 调用失败时，诊断应指出“receiver 是只读 trait object”，而不是继续报旧的“trait is not dyn-compatible”总括错误。

- AC-4: LLVM lowering 能正确表达可写/只读 dyn receiver，并保持 ABI 正确性与对象布局稳定。
  - Positive Tests (expected to PASS):
    - `const Hash dyn` 和 `Hash dyn` 继续使用同一类 `{data_ptr, witness_ptr}` 运行时形状。
    - witness slot 调用能区分 getter slot 与 setter slot 的 receiver constness。
    - setter 动态调用在 native ABI 下对隐式 `self`、`sret` 和间接返回的组合保持正确。
    - ordinary struct layout 保持不变，不向用户 struct 注入 vptr 或额外 mutability tag。
  - Negative Tests (expected to FAIL):
    - 不能通过在 trait object 里额外塞用户可见 mutability flag 来绕过类型系统。
    - 不能为了支持 `set def` dyn dispatch 而回退到“整 trait 两套完全不同的用户类型语法”。
    - 不能破坏已经通过回归验证过的 get-only `Trait dyn` ABI。

- AC-5: 模块接口、增量缓存、诊断与文档同步到“const-aware dyn dispatch”规则。
  - Positive Tests (expected to PASS):
    - 带 `set def` 的 trait 方法签名变化会继续进入 `interfaceHash` 并触发 importer 失效。
    - getter/setter receiver access 变化会触发相关 dyn call importer 重新编译。
    - language reference 和 internals 文档明确区分 `Hash dyn` 与 `const Hash dyn` 的调用边界。
    - acceptance/module/frontend/incremental/smoke 至少各有一条与 mutable dyn 相关的正反例。
  - Negative Tests (expected to FAIL):
    - 不能把 setter dyn call 的行为只写在实现里而不更新文档。
    - 不能让增量缓存继续复用旧的“整个 trait 不支持 dyn”的诊断或旧 lowering 结果。

## Path Boundaries

### Upper Bound (Maximum Scope)

这版计划允许做到：

- `Hash dyn` / `const Hash dyn` 的 receiver mutability 分流
- mixed getter/setter trait 的动态构造与动态调用
- pointer-backed source、`self`、embedded field 等 writable source 的 mutable dyn 构造
- imported trait / imported impl 的 mutable dyn 分发
- 维持当前 witness 模型下的 ABI 正确性

但仍然不进入：

- owning trait object
- `impl Type: Trait { ... }`
- 泛型 / trait bound
- default method
- auto trait / negative impl
- 隐式 boxing

### Lower Bound (Minimum Scope)

最小可接受实现是：

- 语义层支持 `Hash dyn` 与 `const Hash dyn`
- mixed getter/setter trait 不再整体拒绝 dyn
- `const Hash dyn` 只允许 getter
- `Hash dyn` 允许 getter + setter
- 动态 lowering 保持当前 `{data_ptr, witness_ptr}` 模型

如果实现过程中发现 ABI 风险，允许先只支持：

- 局部变量
- 参数
- 返回值

上的 mutable dyn dispatch，再补更复杂的 source 传播场景，但不能回退到“带 `set def` 的 trait 完全不能 dyn”。

### Allowed Choices

- Can use: 现有 `const` 类型修饰规则来表达只读 dyn receiver。
- Can use: 单一 witness table 加调用点 constness 检查。
- Can use: adapter thunk，只要它是 ABI 正确性的内部实现细节。
- Can use: 针对 getter/setter 分别推导 slot function type。
- Cannot use: 新的用户层关键字，例如 `mut dyn` 或 `readonly dyn`。
- Cannot use: 修改普通 struct 的内存布局。
- Cannot use: 把 `Trait.method(value, ...)` 改成动态路径。
- Cannot use: 用“整个 trait 再分成 readonly/mutable 两个 trait 名”规避 receiver mutability 设计。

## Related Paths

- [trait.md](/home/lurker/workspace/compiler/lona/docs/reference/language/trait.md) - 当前稳定用户语义
- [trait_lowering.md](/home/lurker/workspace/compiler/lona/docs/internals/compiler/trait_lowering.md) - 当前 dyn lowering 与 witness 模型
- [function.cc](/home/lurker/workspace/compiler/lona/src/lona/analyze/function.cc) - trait object cast / call 分析与 dyn-compatible 检查
- [type.hh](/home/lurker/workspace/compiler/lona/src/lona/type/type.hh) - `DynTraitType` 与 const-qualified type 组合
- [type.cc](/home/lurker/workspace/compiler/lona/src/lona/type/type.cc) - `DynTraitType` LLVM 形状
- [codegen.cc](/home/lurker/workspace/compiler/lona/src/lona/emit/codegen.cc) - witness table 生成与 trait object lowering
- [test_surface_syntax.py](/home/lurker/workspace/compiler/lona/tests/acceptance/language/test_surface_syntax.py) - 语言 acceptance 正反例
- [test_modules.py](/home/lurker/workspace/compiler/lona/tests/acceptance/modules/test_modules.py) - imported trait / impl 回归
- [test_frontend.py](/home/lurker/workspace/compiler/lona/tests/acceptance/toolchain/test_frontend.py) - frontend IR 断言
- [incremental_smoke.py](/home/lurker/workspace/compiler/lona/tests/incremental_smoke.py) - 接口哈希与会话内复用回归
- [test_system.py](/home/lurker/workspace/compiler/lona/tests/smoke/test_system.py) - 运行时行为回归

## Dependencies and Sequence

### Milestones

1. Milestone 1: 语义规则改造
   - Phase A: 把 dyn-compatible 从 trait 级别规则改成 method-level receiver mutability 规则
   - Phase B: 明确 `Hash dyn` / `const Hash dyn` 的 cast、赋值和调用边界

2. Milestone 2: 分析阶段与 HIR
   - Phase A: `cast[Trait dyn](&value)` 在 mutable / const source 上产出正确类型
   - Phase B: `h.method()` 按具体 trait method 的 receiver access 选择合法/非法路径

3. Milestone 3: Lowering 与 ABI
   - Phase A: 为 getter / setter 动态调用生成正确 slot function type
   - Phase B: 如果直接存真实方法符号不再稳妥，则引入 adapter thunk 保持 ABI 正确

4. Milestone 4: 回归与文档
   - Phase A: acceptance/module/frontend/incremental/smoke 补齐 mutable dyn 覆盖
   - Phase B: reference / internals 文档同步到新的 const-aware dyn 规则

依赖关系：

- 语义规则必须先定清，否则后续 HIR 和 codegen 无法收口。
- method-level 动态调用检查必须先完成，否则 setter dyn lowering 会缺少合法入口。
- ABI 路径需要在测试前稳定，否则 witness slot 与 native ABI 的组合很容易出现隐性错误。

## Task Breakdown

Each task must include exactly one routing tag:
- `coding`: implemented by Claude
- `analyze`: executed via Codex (`/humanize:ask-codex`)

| Task ID | Description | Target AC | Tag (`coding`/`analyze`) | Depends On |
|---------|-------------|-----------|----------------------------|------------|
| task1 | 重新定义 dyn-compatible 语义：允许 mixed getter/setter trait 构造 dyn，并把限制下沉到 receiver mutability 与 method access | AC-1, AC-2 | coding | - |
| task2 | 扩展 cast / type-check / assignment 规则，使 `Hash dyn` 与 `const Hash dyn` 的 const 传播稳定工作 | AC-1 | coding | task1 |
| task3 | 改造 trait object call 分析，让 getter/setter 调用按 dyn receiver 的 constness 进行 targeted 诊断与合法化 | AC-2, AC-3 | coding | task2 |
| task4 | 重新审查并实现 setter dyn dispatch 的 slot function type / ABI 路径，必要时引入 adapter thunk | AC-4 | analyze | task3 |
| task5 | 在 LLVM lowering 中落地 getter/setter 动态调用，保持 witness table 与对象布局边界稳定 | AC-4 | coding | task4 |
| task6 | 补 acceptance/module/frontend/incremental/smoke 回归，覆盖 mutable dyn 构造、getter/setter 调用、imported impl 和缓存失效 | AC-5 | coding | task5 |
| task7 | 更新 language reference 与 internals 文档，明确 `Hash dyn` / `const Hash dyn` 的行为差异 | AC-5 | coding | task6 |

## Feasibility Hints

- 最稳的语义模型不是新增一种“mutable trait object 结构体”，而是继续复用现有 `const` 规则：
  - `Hash dyn`
  - `const Hash dyn`
- `cast[Hash dyn](&value)` 应保留 source 的 `const` 资格；不要再额外要求用户写 `cast[const Hash dyn](...)`。
- `data_ptr` 在类型擦除后仍然要保留 pointee constness：
  - writable source -> `any*`
  - const source -> `any const*`
- setter 是否可调用，优先根据擦除后 `data_ptr` 的可写性判断，而不是根据 `witness_ptr` 单独携带一份 mutability tag。
- witness table 优先继续保持一份 `(Trait, Type)` 一张表。
- getter/setter 的差异优先通过：
  - method declaration 的 `receiverAccess`
  - 调用点的 dyn receiver constness
  - slot function type 的 receiver pointee constness
  来表达。
- 如果 setter dyn dispatch 在 native ABI 上与直接方法符号 ABI 不能稳定对齐，再引入 adapter thunk，不要先入为主地把 thunk 变成强制设计。
- 诊断要从旧的“trait is not dyn-compatible”改成更具体的：
  - trait object is read-only
  - method requires writable receiver
  - cast would drop const

## Pending User Decisions

- DEC-1: setter dyn dispatch 是否允许一步到位支持 pointer-backed source，例如 `&self`、`&ptr`、embedded promoted field
  - Proposed Direction: 应允许，因为这些场景正是 mutable dyn 的高价值用例。
  - Tradeoff Summary: 价值高，但会让 addressability / writability 检查更早碰到边界场景。
  - Decision Status: `PENDING`

## Implementation Notes

- 代码中不要出现“plan task”之类的计划术语。
- 不要把 `set def` dyn dispatch 的语义藏进 lowering；语义约束必须在 analyze 阶段先明确。
- 不能因为要支持 setter dyn dispatch，就回退去修改普通对象布局或让 static trait dispatch 经过 witness table。
