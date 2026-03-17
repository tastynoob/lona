---
name: lona-test-generator
description: Generate and validate focused Lona compiler test cases from the local language docs and test tools.
---

# Lona Test Skill

这个 skill 用来给 `lona` 生成新的测试样例，并在生成后立刻验证它们。

## 目标

- 从现有文档推导出合法的 `.lo` 示例。
- 优先生成小而集中的测试，而不是大而杂的综合样例。
- 每个正向样例都必须能产出通过验证的 LLVM IR，并且能继续被 clang 接受。
- 负向样例只在需要测试诊断时生成，并且要写明期待的错误子串。

## 开始前先读

1. [docs/README.md](../docs/README.md)
2. [tests/README.md](README.md)
3. 按需要再读：
   - [docs/program.md](../docs/program.md)
   - [docs/expr.md](../docs/expr.md)
   - [docs/type.md](../docs/type.md)
   - [docs/grammer.md](../docs/grammer.md)

## 生成规则

- 单个测试尽量控制在 `10-40` 行。
- 默认每个文件只聚焦 `1-3` 个语法点。
- 如果需要模块测试，可以生成同目录的多个 `.lo` 文件。
- 正向样例优先覆盖：
  - `import`
  - `struct` 与方法
  - `f32/f64`
  - tuple
  - pointer
  - function pointer
- 当前不要主动生成这些未完成特性：
  - 字符串运行时行为
  - 数组 lowering
  - placeholder 运算符语义
  - 无上下文 tuple literal

## 验证命令

- 正向样例：
  - `bash tests/tools/compile_case.sh path/to/case.lo`
- 负向样例：
  - `bash tests/tools/expect_diag.sh path/to/case.lo 'expected text'`
- 模板随机样例参考：
  - `python3 tests/template_random.py --compiler build/lona-ir`

## 建议工作流

1. 先选一个小主题，例如 `tuple + float` 或 `import + struct`。
2. 在临时目录或指定输出目录里生成 `.lo` 文件。
3. 每写完一个正向样例，立刻跑 `compile_case.sh`。
4. 每写完一个负向样例，立刻跑 `expect_diag.sh`。
5. 保留最终通过验证的样例，再决定是否纳入仓库。

## 输出要求

- 说明每个样例覆盖了什么。
- 标明用了哪些验证命令。
- 如果某个组合因为当前语言未实现而放弃，要直接写清楚原因。
