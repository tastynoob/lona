# Example Index

`example/` 里的样例按主题拆开，优先从这里找最接近的语言能力。

## Core Language

- [algorithms_suite.lo](algorithms_suite.lo): 递归、循环、分支、布尔值和基础整数运算。
- [c_ffi_linked_list.lo](c_ffi_linked_list.lo): `extern "C"`、`malloc/free/puts`、`u8[*]`、结构体方法和链表增删改查。
- [data_model_suite.lo](data_model_suite.lo): 结构体、字段、方法、构造函数和指针读写。
- [function_pointer_suite.lo](function_pointer_suite.lo): 显式函数取指针、函数指针存储和间接调用。
- [syntax_suite.lo](syntax_suite.lo): `f32/f64`、tuple、固定维度数组、位运算、比较和逻辑短路。

## Modules

- [modules/main.lo](modules/main.lo): 最小模块入口示例。
- [modules/math.lo](modules/math.lo): 被 `main.lo` 导入的模块定义。

## 建议顺序

1. 先看 [algorithms_suite.lo](algorithms_suite.lo) 了解基础控制流。
2. 再看 [data_model_suite.lo](data_model_suite.lo) 和 [function_pointer_suite.lo](function_pointer_suite.lo)。
3. 需要看 system-level `C FFI` 和真实运行样例时，查 [c_ffi_linked_list.lo](c_ffi_linked_list.lo)。
4. 需要看当前语法收口结果时，查 [syntax_suite.lo](syntax_suite.lo)。
5. 需要模块导入时，查 [modules/main.lo](modules/main.lo)。
