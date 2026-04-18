# Proposals

这里收纳仍然有现实参考价值、但还没有收口成稳定文档的设计提案。

这些文档的特点是：

- 可能描述未实现能力
- 可能与当前实现部分不一致
- 可以作为后续设计讨论的起点

当前文档：

- [const_qualifier_v0.md](const_qualifier_v0.md): `const` 类型修饰符草案。
- [extension_method_v0.md](extension_method_v0.md): extension method v0 草案，目标是在不改变 inherent / trait method 优先级的前提下，引入只对当前模块和直接导入模块生效的扩展方法机制。
- [generic_v0.md](generic_v0.md): 泛型 v0 草案，目标是让静态泛型、trait 约束和当前模块化缓存模型形成可落地的第一版组合。
- [inline_v0.md](inline_v0.md): `inline` 编译期常量语义草案，目标是先收口一版只覆盖原生标量与指针的常量值模型。
- [output_artifact_reclassification.md](output_artifact_reclassification.md): 重新按 `bc` / `obj` / `linked-obj` 分类输出产物，为增量编译和后续 LTO 收口稳定 CLI 边界。
- [trait_v0.md](trait_v0.md): trait / static dispatch / dynamic trait object 草案。
- [trait_v0_dyn_mutability.md](trait_v0_dyn_mutability.md): `Trait dyn` 可写性扩展计划，目标是让 `set def` 通过 `Hash dyn` / `Hash const dyn` 分流进入动态分派。
