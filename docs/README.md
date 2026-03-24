# Docs Index

`docs/` 里的文档现在按目录分类，优先从这里查。

## 目录结构

- `docs/language/`: 面向语言使用者的语法与语义文档。
- `docs/design/`: 已实现或待收口的语言设计说明。
- `docs/compiler/`: 编译器架构、pipeline 和名字解析等实现边界。
- `docs/runtime/`: target/runtime 模式、构建、ABI、FFI 文档。
- `docs/plans/`: 暂存计划和未落地草案。

## 语言与语法

- [language/program.md](language/program.md): 程序结构、顶层声明和模块入口。
- [language/grammer.md](language/grammer.md): 语法总览、文法约定，以及当前已接通的 FFI 相关声明入口。
- [language/expr.md](language/expr.md): 表达式、调用、运算符和字面量。
- [language/type.md](language/type.md): 类型系统、函数类型、数组和元组。
- [language/func.md](language/func.md): 函数与方法定义。
- [language/struct.md](language/struct.md): 结构体、字段和方法。
- [language/vardef.md](language/vardef.md): 变量定义与初始化规则。
- [language/controlflow.md](language/controlflow.md): `if`、`for`、`ret` 等控制流。

## 语言设计与约束

- [design/const_qualifier_v0.md](design/const_qualifier_v0.md): `const` 类型修饰符草案；区分类型修饰与定义修饰，收口数组、指针、`u8 const[*]`，以及结构体字段与未来 `readonly` 的边界。
- [design/pointer_reference.md](design/pointer_reference.md): 指针 / 引用 / 值语义的设计收口；当前已实现 `ref` 局部绑定、`ref` 参数和隐式 `ref self`。
- [design/string_bytes_v0.md](design/string_bytes_v0.md): 旧版“字节串而不是字符串类型”草案；保留 `u8 const[N]` / `u8 const[*]` 的早期讨论记录，当前实现已收口到 `u8 const[*]` + 自动 NUL 结尾。

## 编译器与工程结构

- [compiler/compiler_architecture.md](compiler/compiler_architecture.md): 当前编译器架构、核心数据结构和编译流程。
- [compiler/compiler_pipeline.md](compiler/compiler_pipeline.md): compile pipeline 和阶段职责。
- [compiler/name_lookup_consistency.md](compiler/name_lookup_consistency.md): 本地 / imported 模块名字查找不一致问题、当前已落地的统一查询 / 分派模型，以及剩余重构边界。
- [compiler/module_member_resolution.md](compiler/module_member_resolution.md): `module.xxx` 已前移到 `resolve` 阶段消解后的边界说明、bare module 的剩余约束，以及 selector 主路径的约束。

## 构建与运行

- [runtime/target_modes.md](runtime/target_modes.md): `native` / `managed` 目标模式边界。
- [runtime/native_build.md](runtime/native_build.md): `lona-ir`、`lac`、`lac-native` 的构建和运行方式。
- [runtime/system_crt_build_v0.md](runtime/system_crt_build_v0.md): hosted 可执行文件第一阶段草案；先复用系统 CRT，只把 LLVM object emission 纳入 `lona`。
- [runtime/native_abi_v0.md](runtime/native_abi_v0.md): `native` 路线第一版内部 ABI 草案。
- [runtime/c_ffi_v0.md](runtime/c_ffi_v0.md): `lona <-> C` 互操作 v0 的当前已实现子集与剩余限制，收口 `#[extern "C"]`、`#[extern] struct` 和 `#[repr "C"] struct` 的边界。
- [../example/README.md](../example/README.md): 示例程序索引。

## 计划与草案

- [plans/indexable_pointer_plan.tmp.md](plans/indexable_pointer_plan.tmp.md): indexable pointer 语法与 lowering 的早期计划草案。
- [plans/module_object_build_v0.md](plans/module_object_build_v0.md): 模块级 `.o` 增量构建、root shim object 和链接后优化的实施计划。

## 建议查阅顺序

1. 先看 [compiler/compiler_architecture.md](compiler/compiler_architecture.md) 了解整体结构。
2. 再看 [language/grammer.md](language/grammer.md)、[language/expr.md](language/expr.md)、[language/type.md](language/type.md) 了解语言表面语法。
3. 如果涉及 `#[extern "C"]`、`#[extern] struct`、`#[repr "C"] struct`，继续看 [runtime/c_ffi_v0.md](runtime/c_ffi_v0.md)。
4. 需要构建或运行时，查 [runtime/native_build.md](runtime/native_build.md)。
5. 需要区分目标模式时，查 [runtime/target_modes.md](runtime/target_modes.md)。
