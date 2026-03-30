# 本地可执行文件构建

如果你先想知道三个命令各自怎么调用，先看 [commands.md](commands.md)。

工具链提供两条把 `lona` 程序继续编成可执行文件的路径。

支持的平台是：

- Linux x86_64

## 两种构建模式

### 1. System 模式

这是更推荐的系统级可执行文件方案。

思路：

- `lona-ir` 生成模块 object bundle
- 显式 `--lto full` 时，`lona-ir` 会改走 bitcode 全局链接后优化，再发单最终 object
- 只有显式 `--emit ir` 时，`lona-ir` 才会输出文本 LLVM IR
- 最终可执行文件交给系统 linker driver 做多 object 链接
- 进程启动复用系统 CRT 的宿主入口对象
- hosted wrapper 使用标准 `main(argc, argv)` 形态
- 默认 target triple 是 `x86_64-unknown-linux-gnu`

这条路径的优点是：

- 不需要自己接管宿主进程初始化
- `clang` / `cc` 只负责最终链接，不再负责把 LLVM IR 编成目标码
- 更适合作为当前阶段的默认可执行文件方案

### 2. Bare 模式

这是最小裸启动环境。

思路：

- `lona-ir` 生成 bare 用的模块 object bundle
- 自定义 `_start`
- 自定义 linker script
- 不依赖 libc 或系统 CRT
- 默认 target triple 是 `x86_64-none-elf`
- LLVM 会把它规范化打印成 `x86_64-none-unknown-elf`
- `x86_64-unknown-none-elf` 也会按 bare 目标处理

native object 还会额外携带：

- `.lona.native_abi`
  - 只有对象里实际包含 `lona native ABI` 边界时才写入
  - 当前 payload 是 `lona.native_abi=v0.0`
  - ELF 会显式使用 `.lona.native_abi`
  - Mach-O 会改用 `__TEXT,__lona_abi`

## 组成

职责分工：

- `lac`
  - 调用 `lona-ir --emit objects`
  - 读取 manifest，收集模块 object
  - 额外生成 hosted entry object
  - 检查 object bundle 中是否存在 `__lona_main__`
  - 调用系统 linker driver 生成最终程序
  - 如果显式传 `--lto full`，则改走 `lona-ir --emit obj --lto full`
- `lac-native`
  - 调用 `lona-ir --emit objects --target x86_64-none-elf`
  - 汇编启动代码
  - 使用 linker script 把 startup object 和多 object bundle 链接成 ELF 可执行文件
  - 如果显式传 `--lto full`，则改走 `lona-ir --emit obj --lto full`
- bare startup assembly
  - 提供 `_start`
  - 调用稳定入口 `__lona_main__`
  - 把返回值作为进程退出码传给 `exit` syscall
- bare linker script
  - 提供最小 ELF 链接布局
  - 设定 `ENTRY(_start)`

## 程序入口约定

语言层关于 root 模块和顶层可执行语句的规则见 [../language/program.md](../language/program.md)。
这一节只描述构建路径如何消费语言入口。

为了让启动代码和 hosted wrapper 都不依赖路径相关的内部约定，入口分成两层：

- 语言入口：`__lona_main__() -> i32`
- hosted system wrapper：`main(i32 argc, ptr argv) -> i32`

入口选择规则：

1. root 模块的可执行入口仍然固定为 `__lona_main__() -> i32`。
2. hosted target（例如 `x86_64-unknown-linux-gnu`）会额外补一个 `main(argc, argv)`，并把参数保存到 `@__lona_argc` / `@__lona_argv`。
3. bare target（例如 `x86_64-none-elf`）只依赖 `__lona_main__`，不生成 hosted wrapper，也不引入这两个全局。
4. imported 模块如果需要执行顶层语句，编译器会为它们生成内部的模块初始化入口；这些入口不属于稳定外部 ABI。
5. `__lona_main__` 在执行 root 模块自身顶层语句前，会先触发依赖模块的初始化链；每个模块初始化只执行一次，并保留 `i32` 返回值用于向上传播初始化失败。
6. 如果当前程序没有建立 `__lona_main__`，两条构建路径都会报错并提示缺少可执行入口。

如果你要在语言层读取 hosted 参数，现在可以显式声明：

```lona
#[extern]
global __lona_argc i32

#[extern]
global __lona_argv u8 const[*][*]
```

然后按普通全局变量访问，例如 `__lona_argv(1)(0)` 表示第一个命令行参数的第一个字节。

## 使用方式

走 system 路径构建一个可执行文件：

```bash
lac input.lo output/program
```

如果你要走 bare 路径：

```bash
lac-native input.lo output/program
```

如果要覆盖默认 target：

```bash
lona-ir --emit ir --target x86_64-none-elf input.lo output.ll
lona-ir --emit objects --target x86_64-unknown-linux-gnu input.lo output.manifest
lona-ir --emit entry --target x86_64-unknown-linux-gnu hosted-entry.o
lona-ir --emit objects --cache-dir cache/objects --target x86_64-unknown-linux-gnu input.lo output.manifest
lac --lto full input.lo output/program
lac-native --lto full input.lo output/program
lac --target x86_64-unknown-linux-gnu input.lo output/program
lac-native --target x86_64-none-elf input.lo output/program
```

其中：

- `--emit objects` 会写一个 manifest
- 默认 bundle object 会写到 `./lona_cache/output.manifest.d/`
- 如果显式传 `--cache-dir <dir>`，bundle object 会写到 `<dir>/output.manifest.d/`
- 如果显式传 `--no-cache`，本轮会跳过 object cache 复用并强制重新编译模块
- `--emit entry` 会单独生成 hosted `main(argc, argv) -> __lona_main__` object

带优化级别：

```bash
lac -O 2 input.lo output/program
```

显式开启 full LTO：

```bash
lona-ir --emit ir --lto full -O3 input.lo output.ll
lona-ir --emit obj --lto full -O3 input.lo output.o
lac --lto full -O 3 input.lo output/program
```

## 安装后的行为

执行 `make install` 后，会得到：

- `lona-ir`
- `lac`
- `lac-native`

其中：

- `lac` 对应 system 路径
- `lac-native` 对应 bare 路径

`lac` 会优先查找同目录下的 `lona-ir`。

`make install` 不把 bare runtime 资产安装进系统目录。这意味着：

- system 路径不受影响，继续直接复用系统 CRT
- bare 路径如果使用安装后的 `lac-native`，需要用户自己提供 `STARTUP_SRC` 和 `LINKER_SCRIPT`

## 工具依赖

默认依赖：

- `lona-ir`
- `cc`
- `ld`
- `nm`

也可以通过环境变量覆盖：

- `LONA_IR_BIN`
- `LONA_BIN`
- `CC_BIN`
- `LD_BIN`
- `NM_BIN`
- `STARTUP_SRC`
- `LINKER_SCRIPT`
- `TARGET_TRIPLE`

## 边界

这套环境的边界是：

- 只覆盖 Linux x86_64
- system 路径直接复用宿主 ABI 和系统 CRT 启动对象
- bare 路径只支持无 libc 的最小裸链接路径
- bare 启动代码只处理 `i32` 退出码，不处理参数和环境变量
- system 路径只把 `argc/argv` 暴露成 `@__lona_argc` / `@__lona_argv` 两个 extern global；更高级的命令行封装还没有内建
- `--lto full` 只提供 full-LTO 慢路径，还没有 ThinLTO
