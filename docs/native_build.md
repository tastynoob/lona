# 本地可执行文件构建

当前仓库已经提供两条可执行文件构建路径，可以把 `lona-ir` 生成的 LLVM IR 继续编成可运行程序。

当前支持的平台是：

- Linux x86_64

## 两种构建模式

### 1. Hosted 模式

这是当前更推荐的临时方案。

思路：

- `lona-ir` 仍然只负责生成 LLVM IR
- 最终可执行文件交给 clang 负责
- 入口和调用约定暂时套用 clang / C 的宿主 ABI

相关文件：

- [scripts/lac.sh](../scripts/lac.sh)
- [scripts/hosted_smoke.sh](../scripts/hosted_smoke.sh)

这条路径的优点是：

- 不需要自己接管宿主进程初始化
- 可以直接复用 clang 的 IR 编译和系统链接流程
- 更适合作为当前阶段的默认可执行文件方案

### 2. Freestanding 模式

这是当前保留的一版最小裸机式启动环境。

思路：

- `lona-ir` 生成 LLVM IR
- `llc-18` 把 `.ll` 编成 `.o`
- 自定义 `_start`
- 自定义 linker script
- 不依赖 libc 或系统 CRT

## 组成

相关文件：

- [scripts/lac-native.sh](../scripts/lac-native.sh)
- [scripts/native_smoke.sh](../scripts/native_smoke.sh)
- [runtime/linux_x86_64/lona_start.S](../runtime/linux_x86_64/lona_start.S)
- [runtime/linux_x86_64/lona.ld](../runtime/linux_x86_64/lona.ld)

职责分工：

- `lac.sh`
  - 调用 `lona-ir` 生成最终链接后的 LLVM IR
  - 检查 IR 中是否存在 hosted 兼容的 `main`
  - 调用 clang 直接把 IR 链接成可执行文件
- `lac`
  - 这是推荐的安装入口，先产出 LLVM IR，再调用 clang 生成二进制文件
- `lac-native.sh`
  - 调用 `lona-ir` 生成最终链接后的 LLVM IR
  - 调用 `llc-18` 把 `.ll` 编成 `.o`
  - 汇编启动代码
  - 使用自定义 linker script 生成 ELF 可执行文件
- `lona_start.S`
  - 提供 `_start`
  - 调用稳定入口 `__lona_entry__`
  - 把返回值作为进程退出码传给 `exit` syscall
- `lona.ld`
  - 提供最小 ELF 链接布局
  - 设定 `ENTRY(_start)`

## 程序入口约定

为了让启动代码不依赖带路径的内部符号名，最终链接后的 IR 现在会在可行时自动补一个稳定入口：

- `__lona_entry__`
- `main`

入口选择规则：

1. 如果 root 模块存在顶层程序入口 `<root-path>.main`，则 `__lona_entry__` 调用它。
2. 否则，如果 root 模块存在 `def main() i32`，则 `__lona_entry__` 调用它。
3. 如果模块里还没有标准 `main() -> i32`，则会再自动补一个 `main`，让 hosted 构建链可以直接交给 clang。
4. 如果连 `__lona_entry__` 都无法建立，则两条构建路径都会报错并提示当前程序缺少可执行入口。

## 使用方式

先确保编译器已经构建完成：

```bash
make -j4 all
```

然后走 hosted 路径构建一个可执行文件：

```bash
bash scripts/lac.sh input.lo output/program
```

如果你要走 freestanding 路径：

```bash
bash scripts/lac-native.sh input.lo output/program
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

- `lac` 对应 hosted 路径
- `lac-native` 对应 freestanding 路径

`lac` 会优先查找同目录下的 `lona-ir`。`lac-native` 会自动从安装目录下的 `share/lona/runtime/linux_x86_64` 查找启动汇编和链接脚本。

## 工具依赖

当前脚本默认依赖：

- `build/lona-ir`
- `clang-18` 或 clang
- `llc-18` 或 `llc`
- `cc`
- `ld`

也可以通过环境变量覆盖：

- `LONA_IR_BIN`
- `LONA_BIN`
- `CLANG_BIN`
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

以及 hosted smoke：

```bash
make hosted_smoke
```

它会验证两种情况：

- `def main() i32 { ... }` 能作为标准程序入口运行
- 仅含顶层语句的程序也能成功包装成可执行文件并退出 `0`

## 当前边界

这套环境当前是“最小可用”实现，边界比较明确：

- 只覆盖 Linux x86_64
- hosted 路径当前直接复用 clang ABI，后续还可以替换成 lona 自己的 ABI
- freestanding 路径仍然只支持无 libc 的最小裸链接路径
- freestanding 启动代码只处理 `i32` 退出码，不处理参数和环境变量
