# Acceptance Index

`tests/acceptance/` 现在按主题分成四组，结构尽量贴近 `docs/reference/language/` 的文档组织，而不是继续把所有样例平铺在同一层。

## Language

- [language/test_surface_syntax.py](language/test_surface_syntax.py): JSON milestone 节点、多行布局、续行规则，以及几类 parser 级语法边界。
- [language/test_casts.py](language/test_casts.py): builtin cast、数值转换链、`tobits`/`toi32` 和 cast 失败诊断。
- [language/test_sizeof_and_literals.py](language/test_sizeof_and_literals.py): `sizeof`、`usize` 目标宽度、数值字面量前缀/后缀、字符字面量。
- [language/test_strings_and_null.py](language/test_strings_and_null.py): 字符串常量、转义、`null` 指针语义和相关只读/推导约束。
- [language/test_const_qualification.py](language/test_const_qualification.py): `const` 类型物化、只读绑定和 const 存储拒绝路径。
- [language/test_aggregate_types.py](language/test_aggregate_types.py): tuple/array 聚合类型、ABI 形状、view/indexing 与聚合诊断。
- [language/test_struct_construction.py](language/test_struct_construction.py): 取地址、struct 构造、命名调用、字段类型组合与相关报错。
- [language/test_controlflow.py](language/test_controlflow.py): `if` / `for` / `for ... else` / `break` / `continue` 的 JSON、诊断和运行语义。
- [language/test_embedding.py](language/test_embedding.py): struct 嵌入、成员提升、显式路径、构造边界和歧义诊断。
- [language/test_functions.py](language/test_functions.py): 函数指针、C FFI、裸函数限制、方法选择器和调用检查。
- [language/test_operators.py](language/test_operators.py): 算术、位运算、比较、逻辑短路和运行语义。
- [language/test_references.py](language/test_references.py): `ref` 局部绑定、参数传递、addressable 约束与 const 传播。

## Diagnostics

- [diagnostics/test_diagnostics.py](diagnostics/test_diagnostics.py): parser / sema 错误诊断和 targeted help 文案。

## Modules

- [modules/test_modules.py](modules/test_modules.py): 模块导入、跨模块结构体/方法/构造、include path 与模块级语义。

## Toolchain

- [toolchain/test_frontend.py](toolchain/test_frontend.py): AST/JSON、基础 IR、debug IR、target/object/object bundle 语义、跨进程 object cache 复用，以及 `--lto full` 慢路径。
