# Proposals

这里收纳仍然有现实参考价值、但还没有收口成稳定文档的设计提案。

这些文档的特点是：

- 可能描述未实现能力
- 可能与当前实现部分不一致
- 可以作为后续设计讨论的起点

当前文档：

- [const_qualifier_v0.md](const_qualifier_v0.md): `const` 类型修饰符草案。
- [trait_v0.md](trait_v0.md): trait / static dispatch / dynamic trait object 草案。
- [trait_v0_dyn_mutability.md](trait_v0_dyn_mutability.md): `Trait dyn` 可写性扩展计划，目标是让 `set def` 通过 `Hash dyn` / `const Hash dyn` 分流进入动态分派。
- [next_plan.md](next_plan.md): 从 reference 文档中抽离出的后续规划项。
