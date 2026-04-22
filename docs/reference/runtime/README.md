# Runtime Reference

这里收纳当前可直接使用的构建、运行和互操作参考。

- [commands.md](commands.md): `lona-ir`、`lac`、`lac-native` 的命令行入口、参数和常用调用方式。
- [query.md](query.md): `lona-query` 的启动方式、命令、JSON 输出和项目重载语义。
- [native_build.md](native_build.md): `lona-ir`、`lac`、`lac-native` 的构建与运行方式。
- [c_ffi.md](c_ffi.md): 当前 `lona <-> C` 互操作的稳定子集与限制。

说明：

- 这里只放“现在怎么用”。
- 当前已经接入最小 managed 构建入口 `lona-ir --emit mbc`，具体参数见 [commands.md](commands.md)。
- `managed` 运行时实现当前在独立仓库 [lona-MVM](https://github.com/Lona-Lang/lona-MVM)。
- 像目标模式分层、内部 ABI 版本和早期 hosted build 方案，移到了 `docs/internals/` 或 `docs/archive/`。
