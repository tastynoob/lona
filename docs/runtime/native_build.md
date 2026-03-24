# 本地可执行文件构建

当前仓库已经提供两条可执行文件构建路径，可以把 `lona` 程序继续编成可运行程序。

当前支持的平台是：

- Linux x86_64

## 两种构建模式

### 1. System 模式

这是当前更推荐的系统级可执行文件方案。

思路：

- `lona-ir` 直接生成最终 `.o`
- 最终可执行文件交给系统 linker driver
- 进程启动复用系统 CRT 的宿主入口对象
- hosted wrapper 使用标准 `main(argc, argv)` 形态
- 默认 target triple 是 `x86_64-unknown-linux-gnu`

相关文件：

- [scripts/lac.sh](../scripts/lac.sh)
- [tests/smoke/system.sh](../tests/smoke/system.sh)

这条路径的优点是：

- 不需要自己接管宿主进程初始化
- `clang` / `cc` 只负责最终链接，不再负责把 LLVM IR 编成目标码
- 更适合作为当前阶段的默认可执行文件方案

### 2. Bare 模式

这是当前保留的一版最小裸启动环境。

思路：

- `lona-ir` 生成 LLVM IR
- `llc-18` 把 `.ll` 编成 `.o`
- 自定义 `_start`
- 自定义 linker script
- 不依赖 libc 或系统 CRT
- 默认 target triple 是 `x86_64-none-elf`
- LLVM 会把它规范化打印成 `x86_64-none-unknown-elf`
- `x86_64-unknown-none-elf` 也会按 bare 目标处理

当前 native object 还会额外携带：

- `.lona.native_abi`
  - 只有对象里实际包含 `lona native ABI` 边界时才写入
  - 当前 payload 是 `lona.native_abi=v0.0`
  - ELF 会显式使用 `.lona.native_abi`
  - Mach-O 会改用 `__TEXT,__lona_abi`

## 组成

相关文件：

- [scripts/lac-native.sh](../scripts/lac-native.sh)
- [tests/smoke/native.sh](../tests/smoke/native.sh)
- [runtime/bare_x86_64/lona_start.S](../runtime/bare_x86_64/lona_start.S)
- [runtime/bare_x86_64/lona.ld](../runtime/bare_x86_64/lona.ld)

职责分工：

- `lac.sh`
  - 调用 `lona-ir --emit obj --target x86_64-unknown-linux-gnu`
  - 检查对象里是否存在语言入口 `__lona_main__`
  - 调用系统 linker driver 生成可执行文件
- `lac`
  - 这是推荐的安装入口，先产出 `.o`，再调用系统 linker driver 和系统启动对象生成二进制文件
- `lac-native.sh`
  - 调用 `lona-ir --emit ir --target x86_64-none-elf` 生成最终链接后的 LLVM IR
  - 调用 `llc-18` 把 `.ll` 编成 `.o`
  - 汇编启动代码
  - 使用自定义 linker script 生成 ELF 可执行文件
- `lona_start.S`
  - 提供 `_start`
  - 调用稳定入口 `__lona_main__`
  - 把返回值作为进程退出码传给 `exit` syscall
- `lona.ld`
  - 提供最小 ELF 链接布局
  - 设定 `ENTRY(_start)`

## 程序入口约定

为了让启动代码和 hosted wrapper 都不依赖带路径的内部符号名，当前实现把入口分成两层：

- 语言入口：`__lona_main__() -> i32`
- hosted system wrapper：`main(i32 argc, ptr argv) -> i32`

入口选择规则：

1. 如果 root 模块存在顶层可执行语句，它们会直接 lower 到 `__lona_main__() -> i32`。
2. `def main() i32` 现在只是普通函数名，不再自动提升成入口。
3. hosted target（例如 `x86_64-unknown-linux-gnu`）会额外补一个 `main(argc, argv)`，并把参数保存到 `@__lona_argc` / `@__lona_argv`。
4. bare target（例如 `x86_64-none-elf`）只依赖 `__lona_main__`，不生成 hosted wrapper，也不引入这两个全局。
5. 如果连 `__lona_main__` 都无法建立，则两条构建路径都会报错并提示当前程序缺少可执行入口。

## 使用方式

先确保编译器已经构建完成：

```bash
make -j4 all
```

然后走 system 路径构建一个可执行文件：

```bash
bash scripts/lac.sh input.lo output/program
```

如果你要走 bare 路径：

```bash
bash scripts/lac-native.sh input.lo output/program
```

如果要覆盖默认 target：

```bash
lona-ir --emit ir --target x86_64-none-elf input.lo output.ll
lona-ir --emit obj --target x86_64-unknown-linux-gnu input.lo output.o
bash scripts/lac.sh --target x86_64-unknown-linux-gnu input.lo output/program
bash scripts/lac-native.sh --target x86_64-none-elf input.lo output/program
```

带优化级别：

```bash
bash scripts/lac.sh -O 2 input.lo output/program
```

如果你通过 `make install` 安装了工具链，还会得到：

- `lona-ir`
- `lac`
- `lac-native`

其中：

- `lac` 对应 system 路径
- `lac-native` 对应 bare 路径

`lac` 会优先查找同目录下的 `lona-ir`。`lac-native` 会自动从安装目录下的 `share/lona/runtime/bare_x86_64` 查找启动汇编和链接脚本。

## 工具依赖

当前脚本默认依赖：

- `build/lona-ir`
- `llc-18` 或 `llc`
- `cc`
- `ld`

也可以通过环境变量覆盖：

- `LONA_IR_BIN`
- `LONA_BIN`
- `LLC_BIN`
- `CC_BIN`
- `LD_BIN`
- `STARTUP_SRC`
- `LINKER_SCRIPT`
- `TARGET_TRIPLE`

## 测试

仓库内置了 native smoke：

```bash
make native_smoke
```

以及 system smoke：

```bash
make system_smoke
```

它会验证两种情况：

- 顶层语句程序能成功包装成可执行文件
- 通过顶层 `ret run()` 调用普通函数也能作为可执行入口运行

## 当前边界

这套环境当前是“最小可用”实现，边界比较明确：

- 只覆盖 Linux x86_64
- system 路径当前直接复用宿主 ABI 和系统 CRT 启动对象
- bare 路径仍然只支持无 libc 的最小裸链接路径
- bare 启动代码只处理 `i32` 退出码，不处理参数和环境变量
- system 路径当前只把 `argc/argv` 存进 `@__lona_argc` / `@__lona_argv`，还没有对应的语言层访问语法
