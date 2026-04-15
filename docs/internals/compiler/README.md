# Compiler Internals

这里的文档讨论“编译器是怎么做的”，而不是“语言用户应该怎么写”。

- [compiler_architecture.md](compiler_architecture.md): 当前整体架构、核心对象和分层关系。
- [compiler_pipeline.md](compiler_pipeline.md): 单模块 pipeline、阶段职责和代码落点。
- [generic_v0.md](generic_v0.md): generic v0 的 AST、接口模型、runtime instantiation、owner-context 解析与 cache invalidation 边界。
- [name_lookup_consistency.md](name_lookup_consistency.md): 本地 / imported 模块名字查找的一致性边界。
- [module_member_resolution.md](module_member_resolution.md): `module.xxx` 消解前移后的具体边界。
- [query_engine.md](query_engine.md): `lona-query` 复用编译器前端时的分层、`reload` 语义和后续增量方向。
- [symbol_table.md](symbol_table.md): generic + trait 打开后，模块接口表、局部作用域、类型表和 generic instance registry 的分层模型。
- [trait_lowering.md](trait_lowering.md): trait v0 的接口建模、静态分派和 witness-based lowering。
