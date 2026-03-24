# Test Index

`tests/` 里的测试现在分成三类入口。

## Acceptance

- [acceptance/test_frontend.py](acceptance/test_frontend.py): 基于 `pytest` 的 frontend acceptance；覆盖 AST/JSON、基础 IR、debug IR、target/object/object bundle 语义、跨进程 object cache 复用，以及 `--lto full` 慢路径。
- [acceptance/test_controlflow.py](acceptance/test_controlflow.py): 基于 `pytest` 的 `for` / `for ... else` / `break` / `continue` JSON、诊断和运行语义。
- [acceptance/test_functions.py](acceptance/test_functions.py): 基于 `pytest` 的函数指针、C FFI、裸函数限制、方法选择器和调用检查。
- [acceptance/test_diagnostics.py](acceptance/test_diagnostics.py): 基于 `pytest` 的语法和语义错误诊断。
- [acceptance/test_modules.py](acceptance/test_modules.py): 基于 `pytest` 的模块导入、结构体返回、顶层执行与跨模块 C ABI 场景。
- [acceptance/test_operators.py](acceptance/test_operators.py): 基于 `pytest` 的算术、位运算、比较、逻辑短路和运行语义。
- [acceptance/test_references.py](acceptance/test_references.py): 基于 `pytest` 的 `ref` 局部绑定、参数传递与 const 约束。
- [acceptance/test_syntax_features.py](acceptance/test_syntax_features.py): 基于 `pytest` 的 cast、数值转换、字符串、null、tuple、数组、命名调用等语法/语义覆盖。
- [harness/compiler.py](harness/compiler.py): `pytest` 测试的编译调用、`lac.sh` 构建和临时文件辅助。
- [harness/assertions.py](harness/assertions.py): 更短、更定向的文本/对象断言辅助。

## Smoke

- [smoke/test_benchmark.py](smoke/test_benchmark.py): 基于 `pytest` 的编译耗时 smoke benchmark；打印 wall time 和 `--stats` 输出。
- [smoke/test_examples.py](smoke/test_examples.py): 基于 `pytest` 的样例编译 smoke，并实际运行 system-level `C FFI` 示例，锁住文档样例可用性。
- [smoke/test_system.py](smoke/test_system.py): 基于 `pytest` 的 `lac` hosted 路线 smoke，覆盖入口包装、C harness 互调、system-level C FFI 场景，以及 `--lto full` slow path。
- [smoke/test_native.py](smoke/test_native.py): 基于 `pytest` 的 `lac-native` bare 路线 smoke，也覆盖 `--lto full` slow path。
- [template_random.py](template_random.py): 模板随机拼接测试；生成支持的语法组合并验证 LLVM IR 和 clang object 编译。

## Perf

- [perf/generate_large_case.py](perf/generate_large_case.py): 生成固定的 10w 行 perf 样例。
- [perf/profile_large_case.py](perf/profile_large_case.py): 生成固定大样例，跑 `perf record`，然后直接打开 `perf report`。
- [perf/large_case_manifest.json](perf/large_case_manifest.json): 大样例的固定版本、行数和哈希。
- [perf/README.md](perf/README.md): perf 样例覆盖范围、入口和使用方式。

## Incremental

- [incremental_smoke.py](incremental_smoke.py): 同一 `CompilerSession` 内的增量编译测试；当前也覆盖模块级 object / bitcode 发射与复用统计。
- [session_runner.cc](session_runner.cc): Python smoke 用的会话驱动器。
- [pass_fail.py](pass_fail.py): Python pass/fail 输出辅助。
- [fixtures/acceptance_main.lo](fixtures/acceptance_main.lo): acceptance 主 fixture。

## Tools

- [tools/compile_case.py](tools/compile_case.py): 测试辅助脚本；将 `.lo` 编译成验证过的 LLVM IR，并继续生成单最终 `.o`。这是 tests/helper 路径，不代表默认构建链。
- [tools/expect_diag.py](tools/expect_diag.py): 断言某个 `.lo` 编译失败并包含指定诊断子串。
- [test_skill.md](test_skill.md): AI 生成测试样例时应遵守的约束和工作流。
- [test_agent.py](test_agent.py): 调用 `codex` 按 `test_skill.md` 自动生成并验证测试样例的入口。
