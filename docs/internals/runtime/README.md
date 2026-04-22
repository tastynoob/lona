# Runtime Internals

这里的文档描述目标模型和内部调用边界。

- [target_modes.md](target_modes.md): `native` / `managed` 的目标模式分层。
- [native_abi_v0.md](native_abi_v0.md): 当前 `lona native` 路线的内部 ABI 草案与实现边界。

说明：

- `target_modes.md` 是编译模型文档，不是“当前都已经实现”的功能清单。
- 当前最小 managed 构建入口已经在本仓库接通；托管运行时实现当前在独立仓库 [lona-MVM](https://github.com/Lona-Lang/lona-MVM)。
- 更偏历史过程的 hosted build v0 文档已移到 `docs/archive/`。
