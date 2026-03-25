# Compiler Internals

这里的文档讨论“编译器是怎么做的”，而不是“语言用户应该怎么写”。

- [compiler_architecture.md](compiler_architecture.md): 当前整体架构、核心对象和分层关系。
- [compiler_pipeline.md](compiler_pipeline.md): 单模块 pipeline、阶段职责和代码落点。
- [name_lookup_consistency.md](name_lookup_consistency.md): 本地 / imported 模块名字查找的一致性边界。
- [module_member_resolution.md](module_member_resolution.md): `module.xxx` 消解前移后的具体边界。
