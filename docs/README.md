# Docs Index

`docs/` 里的文档现在按主题整理成一组固定入口，优先从这里查。

## 语言与语法

- [program.md](program.md): 程序结构、顶层声明和模块入口。
- [grammer.md](grammer.md): 语法总览和文法约定。
- [expr.md](expr.md): 表达式、调用、运算符和字面量。
- [type.md](type.md): 类型系统、函数类型、数组和元组。
- [func.md](func.md): 函数与方法定义。
- [struct.md](struct.md): 结构体、字段和方法。
- [vardef.md](vardef.md): 变量定义与初始化规则。
- [controlflow.md](controlflow.md): `if`、`for`、`ret` 等控制流。

## 语言设计与约束

- [pointer_reference.md](pointer_reference.md): 指针 / 引用 / 值语义的设计收口；当前已实现 `ref` 局部绑定、`ref` 参数和隐式 `ref self`。
- [string_bytes_v0.md](string_bytes_v0.md): “字节串而不是字符串类型”的草案；收口 `u8[N]` / `u8[*]`、不引入 `char` / `string` / 内建 `Bytes`。

## 编译器与工程结构

- [compiler_architecture.md](compiler_architecture.md): 当前编译器架构、核心数据结构和编译流程。
- [compiler_pipeline.md](compiler_pipeline.md): compile pipeline 和阶段职责。
- [name_lookup_consistency.md](name_lookup_consistency.md): 本地 / imported 模块名字查找不一致问题、当前已落地的统一查询 / 分派模型，以及剩余重构边界。
- [module_member_resolution.md](module_member_resolution.md): `module.xxx` 已前移到 `resolve` 阶段消解后的边界说明、bare module 的剩余约束，以及 selector 主路径的约束。
- [target_modes.md](target_modes.md): `native` / `managed` 目标模式边界。

## 构建与运行

- [native_build.md](native_build.md): `lona-ir`、`lac`、`lac-native` 的构建和运行方式。
- [native_abi_v0.md](native_abi_v0.md): `native` 路线第一版内部 ABI 草案。
- [c_ffi_v0.md](c_ffi_v0.md): `lona <-> C` 互操作第一版草案，收口 `extern "C"`、opaque struct 和 `repr(C)` 的边界。
- [../example/README.md](../example/README.md): 示例程序索引。

## 建议查阅顺序

1. 先看 [compiler_architecture.md](compiler_architecture.md) 了解整体结构。
2. 再看 [grammer.md](grammer.md)、[expr.md](expr.md)、[type.md](type.md) 了解语言表面语法。
3. 需要构建或运行时，查 [native_build.md](native_build.md)。
4. 需要区分目标模式时，查 [target_modes.md](target_modes.md)。
