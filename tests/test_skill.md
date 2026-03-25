---
name: lona-test-generator
description: Generate and validate focused Lona compiler test cases from the local language docs and test tools.
---

# Lona Test Skill

这个 skill 用来给 `lona` 生成新的测试样例，并在生成后立刻验证它们。

## 目标

- 从现有文档推导出合法的 `.lo` 示例。
- 先理解现有 acceptance 覆盖，再优先补边缘路径、怪异组合和脆弱交互，而不是重复 happy-path。
- 优先生成小而集中的测试，而不是大而杂的综合样例。
- 每个正向样例都必须能产出通过验证的 LLVM IR，并且能继续被 clang 接受。
- 负向样例只在需要测试诊断时生成，并且要写明期待的错误子串。
- 如果发现当前编译器存在意料外缺陷，保留最小 repro，而不是把它“修饰”成普通样例。

## 开始前先读

1. [docs/README.md](../docs/README.md)
2. [tests/README.md](README.md)
3. 浏览现有 acceptance：
   - [acceptance/test_frontend.py](acceptance/test_frontend.py)
   - [acceptance/test_functions.py](acceptance/test_functions.py)
   - [acceptance/test_modules.py](acceptance/test_modules.py)
   - [acceptance/test_references.py](acceptance/test_references.py)
   - [acceptance/test_syntax_features.py](acceptance/test_syntax_features.py)
   - [acceptance/test_operators.py](acceptance/test_operators.py)
   - [acceptance/test_diagnostics.py](acceptance/test_diagnostics.py)
4. 按需要再读：
   - [docs/reference/language/program.md](../docs/reference/language/program.md)
   - [docs/reference/language/expr.md](../docs/reference/language/expr.md)
   - [docs/reference/language/type.md](../docs/reference/language/type.md)
   - [docs/reference/language/grammar.md](../docs/reference/language/grammar.md)

## 生成规则

- 单个测试尽量控制在 `10-40` 行。
- 默认每个文件只聚焦 `1-3` 个语法点。
- 如果需要模块测试，可以生成同目录的多个 `.lo` 文件。
- 优先选这些更容易漏掉的问题形态：
  - 边界数值：signed min/max、不同进制、分隔符、显式后缀和上下文类型交互
  - `const` / pointer / indexable pointer / array 的嵌套组合
  - tuple / struct / selector / named arg / cast / control-flow 的奇怪交叉
  - parse 合法但语义分支很脆弱的组合
  - 很短但“反直觉”的输入，而不是正常示例程序
- 正向样例优先覆盖“难一点但文档允许”的组合，不要优先重复这些已经显眼覆盖过的基础路径：
  - 单纯 `import`
  - 单纯 `struct` 与方法
  - 单纯 `f32/f64`
  - 单纯 tuple
  - 单纯 pointer
  - 单纯 function pointer
- 负向样例优先选择：
  - 诊断子串稳定、容易断言
  - 多特性交叉处的报错边界
  - 容易和现有规则产生歧义的输入
- 当前不要主动生成这些未完成特性：
  - 字符串运行时行为
  - 数组 lowering
  - placeholder 运算符语义
  - 无上下文 tuple literal

## 验证命令

- 正向样例：
  - `python3 tests/tools/compile_case.py path/to/case.lo`
  - 这条是 tests/helper 路径，会生成单最终 `.o`，不要把它当成默认构建链语义
- 负向样例：
  - `python3 tests/tools/expect_diag.py path/to/case.lo 'expected text'`
- 模板随机样例参考：
  - `python3 tests/template_random.py --compiler build/lona-ir`

## 建议工作流

1. 先快速排除现有 acceptance 已经明显覆盖过的普通 happy-path。
2. 选一个小主题，但尽量是边缘组合，例如 `signed min + unary minus`、`const pointer + named call`、`tuple + cast + control-flow`。
3. 优先写最小化 repro 风格样例，而不是示例程序风格样例。
4. 在临时目录或指定输出目录里生成 `.lo` 文件。
5. 每写完一个正向样例，立刻跑 `compile_case.py`。
6. 每写完一个负向样例，立刻跑 `expect_diag.py`。
7. 如果碰到意外 bug，保留最小复现并在最终结论里单列说明。
8. 保留最终通过验证的样例，再决定是否纳入仓库。

## 输出要求

- 最终回复只保留简洁结论，不要输出中间过程。
- 说明每个样例覆盖了什么，并列出生成出的样例文件。
- 给出整体结果，以及负向样例命中的诊断结论。
- 如果发现意外 bug 或可疑行为，要单列 repro 文件和现象。
- 如果某个组合因为当前语言未实现而放弃，要直接写清楚原因。
- 除非执行失败，不要罗列 shell 命令或逐步操作日志。
