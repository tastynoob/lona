# Docs Index

`docs/` 按“稳定参考 / 内部实现 / 提案 / 存档”四层组织。

- `reference/`
  当前行为与当前工具链入口，优先给语言使用者和日常查阅使用。
- `internals/`
  编译器实现、模块边界、ABI 和目标模型，优先给维护者和贡献者使用。
- `proposals/`
  还没有收口成稳定文档的设计草案。
- `archive/`
  历史计划、早期方案和已被后续实现覆盖的文档；保留作背景记录，不应作为当前语义依据。

## 建议查阅顺序

1. 语言和语法：看 [reference/language/README.md](reference/language/README.md)
2. 构建、运行和 C FFI：看 [reference/runtime/README.md](reference/runtime/README.md)
3. 编译器架构和实现边界：看 [internals/compiler/README.md](internals/compiler/README.md)
4. 目标模式和内部 ABI：看 [internals/runtime/README.md](internals/runtime/README.md)
5. 需要了解未落地方案时，再看 [proposals/README.md](proposals/README.md)
6. 只在做历史追溯时查看 [archive/README.md](archive/README.md)

## 目录索引

- [reference/README.md](reference/README.md): 稳定参考文档入口。
- [internals/README.md](internals/README.md): 编译器与运行时内部实现文档入口。
- [proposals/README.md](proposals/README.md): 当前仍处于草案状态的提案入口。
- [archive/README.md](archive/README.md): 历史方案、旧计划和已过时设计记录入口。
