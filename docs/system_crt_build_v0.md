# System CRT Build v0

> 这是一份 `lona` hosted 可执行文件构建链的第一版草案。
> 目标不是一步取代系统 linker / CRT，而是先去掉 “clang 负责把 LLVM IR 编成机器码” 这一环，
> 同时继续复用宿主系统已经提供的启动对象、`libc` 和链接约定。

## 1. 目标

当前 `system` 路线是：

- `lona-ir` 生成最终链接后的 LLVM IR
- `clang -x ir` 负责把 IR 编成目标码并完成系统链接

第一阶段计划改成：

- `lona-ir` 自己把最终 LLVM module 编成 `.o`
- `lona` 继续复用宿主系统 CRT
- 最终链接先继续交给系统 linker driver

也就是说，这一阶段去掉的是：

- `clang` 作为 LLVM IR 编译器

这一阶段暂时**不**去掉：

- 系统 CRT
- 系统 libc
- 系统 linker driver

## 2. 非目标

`System CRT Build v0` 暂时不解决：

- 自定义 hosted `_start`
- 自己维护 glibc / musl startup 汇编
- `argc/argv/envp` 的语言层访问语法
- 跨平台 CRT 抽象
- 直接调用 `ld` / `lld` 时的完整 `crt1.o` / `crti.o` / `crtn.o` 发现逻辑
- 动态装载器、PIE、TLS 等完整宿主启动细节

这些内容都可以放到后续阶段。

## 3. 设计原则

### 3.1 先替换 IR 编译，再替换宿主启动

当前最稳定的切入点不是“立刻自己写 hosted 启动汇编”，而是：

1. 保持系统 CRT 不变
2. 先让 `lona-ir` 直接产 `.o`
3. 再把 `.o` 交给宿主链接链

这样收益最大，风险最小。

### 3.2 internal entry 和 hosted entry 分离

编译器内部应该有一个稳定语言入口：

- `i32 @__lona_main__()`

宿主可执行文件入口则由 hosted profile 负责适配：

- `i32 @main(i32 argc, ptr argv)`

因此：

- `__lona_main__` 是语言内部契约
- `main(argc, argv)` 是 system CRT profile 的宿主适配层

不要再把两者混成同一个概念。

### 3.3 hosted v0 不需要自定义启动汇编

只要继续复用系统 CRT，这一阶段就不应该引入自定义 `_start`。

更合适的做法是：

- 系统 CRT 负责 `_start`
- 系统 CRT 负责进程初始化、`libc` 初始化和 `exit`
- `lona` 只提供一个 host-compatible `main`

所以，当前阶段真正需要新增的不是 hosted 启动汇编，而是：

- object emission
- hosted `main` wrapper 约定
- system link profile

## 4. 当前状态

当前仓库已经有两条构建链：

- [scripts/lac.sh](../scripts/lac.sh)
  - system 路线
  - 当前默认 target 是 `x86_64-unknown-linux-gnu`
  - 当前已经走 `lona-ir --emit obj --target ... -> cc`
- [scripts/lac-native.sh](../scripts/lac-native.sh)
  - bare 路线
  - 当前默认 target 是 `x86_64-none-elf`
  - 当前走 `lona-ir --emit ir --target ... -> llc -> startup.o + ld`

同时仓库已经有一套 bare runtime asset：

- [runtime/bare_x86_64/lona_start.S](../runtime/bare_x86_64/lona_start.S)
- [runtime/bare_x86_64/lona.ld](../runtime/bare_x86_64/lona.ld)

这说明：

- bare 路线已经证明“runtime profile + 启动资产”是合理分层
- hosted 路线暂时没有必要立刻复制这套 `_start` 模型

## 5. v0 构建模型

### 5.1 总体链路

建议把 hosted `system` 构建链收口成：

```text
source
  -> AST / resolve / HIR
  -> linked LLVM module
  -> lona object emission
  -> program.o
  -> system linker driver
  -> executable
```

其中：

- `lona-ir` 负责直到 `program.o`
- 最终 executable 先仍由系统工具链完成

### 5.2 object emission

`lona-ir` 需要新增一种输出能力：

- 把最终链接后的 LLVM module 直接发射为 object file

实现上建议直接走 LLVM `TargetMachine`：

- target triple
- data layout
- codegen file type = object
- 复用现有 optimize / verify 之后的最终 module

这一步应当进入 `native lowering` 的正式职责，而不是继续外包给 `llc` 或 `clang`。

### 5.3 hosted 入口包装

对 system CRT profile，编译器应在可执行入口可建立时自动补一个 wrapper：

```text
main(argc, argv) -> store globals -> __lona_main__()
```

第一阶段建议：

- root 模块的顶层可执行语句直接 lower 到 `__lona_main__() -> i32`
- `def main() i32` 现在只是普通函数名，不再自动提升成程序入口
- hosted profile 再生成 C ABI `main(i32 argc, ptr argv)`

推荐的最小 hosted wrapper 语义：

- 把 `argc` 写入 `@__lona_argc`
- 把 `argv` 写入 `@__lona_argv`
- 直接调用 `__lona_main__`
- 直接返回其 `i32` 结果

也就是说，系统 CRT v0 会先保存宿主进程参数，但还不急着把它们设计成语言层公开语法。

### 5.4 linking

第一阶段建议继续复用系统 linker driver，例如：

- `cc`
- `gcc`
- `clang`

这里的重点是：

- 它只负责最终链接
- 不再负责把 LLVM IR 编译成 object

也就是说，允许：

```text
lona-ir --emit obj input.lo program.o
cc program.o -o program
```

即使系统上的 `cc` 最终指向 clang，这个阶段也已经达成了“clang 不再是 LLVM IR 编译器”的目标。

## 6. profile 设计

建议把 hosted 路线显式建成 runtime / link profile：

- `linux-x86_64-system-crt`
- 后续再有 `linux-x86_64-bare`
- 将来还可以有 `linux-aarch64-system-crt`

对 `linux-x86_64-system-crt`，profile 至少要定义：

- target triple
- object format
- symbol naming / ABI
- 是否需要 hosted `main`
- 默认链接策略

这一层的意义是：

- 把“目标平台选择”从脚本字符串里提升成正式编译契约
- 避免未来 `bare/system`、`glibc/musl`、`x86_64/aarch64` 全都堆进一个脚本

## 7. CLI 草案

第一阶段建议给 `lona-ir` 新增：

- `--emit obj`

行为：

- 输入：`input.lo`
- 输出：最终链接后的 root object file
- 仍然执行现有多模块加载、链接、优化和验证

配套脚本可以先收口成：

- `lac`
  - `lona-ir --emit obj`
  - 然后调用系统 linker driver
- `lac-native`
  - 暂时保持现状

后续如果要继续收口，可再考虑：

- `--emit-exe`
- `--target-profile=linux-x86_64-system-crt`

但这些不必和 v0 同时推进。

## 8. 编译器结构草案

### 8.1 `SessionOptions`

当前 [session_types.hh](../src/lona/driver/session_types.hh) 只有：

- `AstJson`
- `LLVMIR`

后续建议扩成：

- `AstJson`
- `LLVMIR`
- `ObjectFile`

如果未来真的把最终链接也纳入编译器，再考虑：

- `Executable`

### 8.2 pipeline

当前 [compiler_pipeline.md](compiler_pipeline.md) 的默认阶段在 `print-llvm` 结束。

引入 object emission 后，建议在逻辑上新增：

1. `emit-llvm`
2. `optimize-llvm`
3. `verify-llvm`
4. `emit-object`

其中：

- `--emit ir` 停在 `print-llvm`
- `--emit obj` 停在 `emit-object`

这样比把 object emission 塞进外部脚本更清楚。

### 8.3 entry synthesis

建议把 entry synthesis 明确分两层：

1. language entry synthesis
   - 目标是补 `__lona_entry__`
2. hosted entry synthesis
   - 目标是补 C ABI `main`

这样 future bare/system/managed 路线都能复用同一个内部入口模型。

## 9. 测试草案

第一阶段至少应补三类测试：

1. object emission smoke
   - `lona-ir --emit obj input.lo output.o`
   - 检查 object file 可生成

2. hosted system smoke
   - `lona-ir --emit obj`
   - 再由 `cc` 链接
   - 验证顶层程序入口和 `ret run()` 这种显式顶层调用都能运行

3. symbol contract test
   - 验证最终 object 中存在 hosted `main`
   - 验证 wrapper 最终调用 `__lona_entry__`

## 10. 风险与后续

### 10.1 当前阶段的主要风险

- `main` 包装规则和现有自动入口生成逻辑可能重叠
- 目标 triple / data layout / relocation model 需要和链接器预期一致
- system link 如果继续走 `cc`，不同发行版的默认 PIE / hardening 选项可能带来细节差异

### 10.2 后续阶段

完成 `System CRT Build v0` 之后，再考虑：

1. 去掉外部 `llc`
   - bare 路线也改成 `lona-ir` 直接产 `.o`

2. 引入正式 target profile
   - hosted / bare 统一进同一套 profile 模型

3. 决定 hosted 是否仍复用系统 linker driver
   - 或改为 `ld` / `lld` + 显式 CRT 资产发现

4. 再决定是否需要自定义 hosted startup 汇编

## 11. 结论

如果当前目标只是：

- 让 `lona` 不再依赖 clang 编译 LLVM IR
- 同时尽快保留可用的 hosted 可执行文件链路

那么最合理的第一步不是引入新的 hosted `_start` 汇编，而是：

- 保留系统 CRT
- 统一内部入口为 `__lona_entry__`
- 由 hosted profile 自动补 `main`
- 让 `lona-ir` 直接产 `.o`
- 最终链接先继续交给系统工具链

这条路线能把“编译职责前移到 `lona`”与“宿主启动继续复用系统能力”清楚分开，工程风险最可控。
