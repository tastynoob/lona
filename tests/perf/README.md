# Perf Tests

`tests/perf/` 放单独的大样例和性能分析入口，不并入常规 smoke benchmark。

- [generate_large_case.py](generate_large_case.py): 生成固定的 10w 行 perf 样例。样例是确定性的，受 [large_case_manifest.json](large_case_manifest.json) 约束，哈希变化必须显式更新 manifest。
- [profile_large_case.py](profile_large_case.py): 生成大样例，用 GNU `perf record` 采样编译过程，然后直接打开 `perf report`。
- [large_case_manifest.json](large_case_manifest.json): 固定样例的版本、行数和 SHA-256。

当前大样例会覆盖这些路径：

- 结构体定义、默认构造、字段读写、成员函数和 `self`
- 普通函数调用、函数指针获取与间接调用
- `ref` 绑定、`ref` 形参与显式 `ref` 实参
- 指针、取地址、解引用、数组指针
- 固定维数组、多维数组、tuple、dot 成员访问
- `if`、`else`、`for`、`ret`、顶层执行语句
- 整数表达式、位运算、比较、逻辑短路、浮点转换和 `tobits`

唯一入口：

```sh
make perf
```

流程固定为三步：

1. 生成固定的 `fixed-large-100k.lo`
2. 用 `perf record` 运行 `lona-ir --emit ir --verify-ir`
3. 直接打开 `perf report`，查看热点函数和调用栈

工作目录固定在 `build/perf/profile-large-case/`，其中至少会留下：

- `fixed-large-100k.lo`
- `perf.data`
- `perf.record.stderr`

如果当前机器的 `perf_event_paranoid` 或 capabilities 不允许 perf events，`make perf` 会在 `perf record` 阶段失败，并保留样例与 stderr 供排查。

如果需要在非交互环境里看纯文本报告，可以直接运行：

```sh
python3 tests/perf/profile_large_case.py \
  --compiler build/lona-ir \
  --workdir build/perf/profile-large-case \
  --report-stdio
```
