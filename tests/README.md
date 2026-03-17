# Test Index

`tests/` 里的测试现在分成三类入口。

## Acceptance

- [acceptance/run.sh](acceptance/run.sh): acceptance 总入口。
- [acceptance/frontend.sh](acceptance/frontend.sh): AST/JSON、基础 IR、debug IR、基本语义。
- [acceptance/functions.sh](acceptance/functions.sh): 函数指针、裸函数类型限制、方法选择器、调用检查。
- [acceptance/diagnostics.sh](acceptance/diagnostics.sh): 语法和语义错误诊断。
- [acceptance/modules.sh](acceptance/modules.sh): 模块导入、结构体返回、顶层执行。
- [acceptance/syntax_features.sh](acceptance/syntax_features.sh): float、tuple、数组占位、运算符占位、旧语法拒绝。
- [acceptance/lib.sh](acceptance/lib.sh): acceptance 公共环境和断言辅助。

## Smoke

- [smoke/benchmark.sh](smoke/benchmark.sh): 编译耗时 smoke benchmark。
- [smoke/hosted.sh](smoke/hosted.sh): `lac` hosted 路线 smoke。
- [smoke/native.sh](smoke/native.sh): `lac-native` freestanding 路线 smoke。
- [template_random.py](template_random.py): 模板随机拼接测试；生成支持的语法组合并验证 LLVM IR 和 clang object 编译。

## Incremental

- [incremental_smoke.py](incremental_smoke.py): 同一 `CompilerSession` 内的增量编译测试。
- [session_runner.cc](session_runner.cc): Python smoke 用的会话驱动器。
- [pass_fail.py](pass_fail.py): Python pass/fail 输出辅助。
- [fixtures/acceptance_main.lo](fixtures/acceptance_main.lo): acceptance 主 fixture。

## Tools

- [tools/compile_case.sh](tools/compile_case.sh): 将 `.lo` 编译成验证过的 LLVM IR，并继续交给 clang 编译成 `.o`。
- [tools/expect_diag.sh](tools/expect_diag.sh): 断言某个 `.lo` 编译失败并包含指定诊断子串。
- [test_skill.md](test_skill.md): AI 生成测试样例时应遵守的约束和工作流。
- [test_agent.sh](test_agent.sh): 调用 `codex` 按 `test_skill.md` 自动生成并验证测试样例的入口。
