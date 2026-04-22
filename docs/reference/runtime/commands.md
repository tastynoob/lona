# 命令行工具使用

构建 / 编译工具链当前对外提供三条主命令：

- `lona-ir`
- `lac`
- `lac-native`

`lona-query` 属于静态分析 / 查询工具，不放在本文展开；单独见 [query.md](query.md)。

本文示例统一使用命令名。如果你的环境通过包装脚本、别名或其它启动方式暴露这些命令，参数语义保持一致。

## 1. 命令可用方式

常见使用方式有两种：

- 安装后直接调用：
  - `lona-ir`
  - `lac`
  - `lac-native`
- 从源码树内使用：
  - 先执行 `make -j4 all`
  - 再通过本地包装脚本或构建产物调用同名命令

`make install` 只安装命令本身。

- `lac` 继续直接复用系统 CRT
- bare runtime 资产不随安装一起分发
- 安装后的 `lac-native` 需要用户显式提供：
  - `STARTUP_SRC`
  - `LINKER_SCRIPT`

## 2. `lona-ir`

`lona-ir` 是底层编译入口。它负责把 `.lo` 源码编成：

- AST JSON
- LLVM IR
- 模块 bitcode bundle
- 模块 object bundle
- 单最终 linked bitcode
- 单最终 object
- hosted entry object

### 2.1 最常见用法

把源码打印成 AST JSON：

```bash
lona-ir input.lo
```

查看当前语言版本和编译器修订版本：

```bash
lona-ir --version
```

输出验证过的 LLVM IR：

```bash
lona-ir --emit ir --verify-ir input.lo
```

把 LLVM IR 写到文件：

```bash
lona-ir --emit ir --verify-ir input.lo output.ll
```

生成模块 bitcode bundle manifest：

```bash
lona-ir --emit bc --verify-ir input.lo output.manifest
```

生成模块 object bundle manifest：

```bash
lona-ir --emit obj --verify-ir input.lo output.manifest
```

生成单最终 linked bitcode：

```bash
lona-ir --emit linked-bc --verify-ir -O3 input.lo output.bc
```

显式走 full-LTO 并输出单最终 object：

```bash
lona-ir --emit linked-obj --lto full --verify-ir -O3 input.lo output.o
```

为 hosted system 路径单独生成 entry object：

```bash
lona-ir --emit entry --target x86_64-unknown-linux-gnu hosted-entry.o
```

### 2.2 输出模式

- 不带 `--emit`
  - 默认输出 AST JSON
- `--emit ir`
  - 输出 LLVM IR
  - 不会自动补 hosted `main(argc, argv)` wrapper
- `--emit bc`
  - 输出模块 bitcode bundle manifest
- `--emit obj`
  - 输出模块 object bundle manifest
- `--emit linked-bc`
  - 输出单最终 linked bitcode
  - 不会自动补 hosted `main(argc, argv)` wrapper
  - 模块级中间产物默认以 bitcode 形式缓存到 `./lona_cache/`
- `--emit mbc`
  - 输出单最终 managed linked bitcode
  - 当前输出内容和 `linked-bc` 一样，但会开启 managed 编译模式
  - 当前 managed 模式只额外限制两类指针操作：任何涉及 `T*` / `T[*]` 的 `cast[T](...)` 都会报错；对 `T[*]` 元素取地址，例如 `&items(0)`，也会报错
  - 函数调用仍按普通 pointer 传参，不做额外托管态传播
  - 模块级中间产物默认以 bitcode 形式缓存到 `./lona_cache/`
- `--emit linked-obj`
  - 输出单最终 object
  - 不会自动补 hosted `main(argc, argv)` wrapper
  - 模块级中间产物默认以 bitcode 形式缓存到 `./lona_cache/`
- `--emit entry`
  - 输出 hosted `main(argc, argv)` wrapper object

### 2.3 常用参数

- `--target <triple>`
  - 指定目标 triple，例如 `x86_64-unknown-linux-gnu`
- `--version`
  - 打印当前版本号，格式为 `语言版本 + 修订版本`
  - 当前语言版本固定为 `0.1 beta`
- `-I <dir>` / `--include-dir <dir>`
  - 追加模块 root 搜索目录
  - 解析 `import` 时，搜索根是“root 源文件所在目录 + 所有 `-I` roots”
  - `-I` roots 不能彼此重叠；不同 roots 也不能导出同一个 canonical 模块路径
- `-O <0-3>`
  - 指定 LLVM 优化级别
- `--verify-ir`
  - 在输出前验证 LLVM IR
- `--lto <off|full>`
  - 选择 link-time optimization 模式
- `--cache-dir <dir>`
  - 对 `--emit bc` / `--emit obj` 生效时，指定 bundle 成员目录根
  - 对 `--emit linked-bc` / `--emit mbc` / `--emit linked-obj` 生效时，指定模块 bitcode 中间缓存目录
- `--no-cache`
  - 禁用本轮模块 artifact 复用
- `-g`
  - 生成 LLVM debug metadata
- `--stats`
  - 向 stderr 打印分阶段统计

### 2.4 参数边界

- `--emit bc` 必须显式提供 manifest 输出路径
- `--emit bc` 不支持 `--lto full`
- `--emit obj` 必须显式提供 manifest 输出路径
- `--emit obj` 不支持 `--lto full`
- `--emit linked-bc` 支持 `--lto off|full`
- `--emit linked-bc` 如果没有显式传 `--cache-dir`，会默认把模块 bitcode cache 写到 `./lona_cache/`
- `--emit mbc` 支持 `--lto off|full`
- `--emit mbc` 如果没有显式传 `--cache-dir`，会默认把模块 bitcode cache 写到 `./lona_cache/`
- `--emit linked-obj` 支持 `--lto off|full`
- `--emit linked-obj` 如果没有显式传 `--cache-dir`，会默认把模块 bitcode cache 写到 `./lona_cache/`
- `--emit entry` 只接受输出 object 路径，不接受输入源码路径
- `--emit entry` 只支持 hosted target；bare target 会直接拒绝
- `--emit entry` 不支持 `--lto full`
- `--emit ir` / `--emit linked-bc` / `--emit mbc` / `--emit linked-obj` 都只包含用户代码与语言入口 `__lona_main__`
- 只要带上编译相关参数，例如 `--target`、`--verify-ir`、`--lto`，默认输出模式就会切到 LLVM IR，而不是 AST JSON

## 3. `lac`

`lac` 是 hosted system 路径的可执行文件构建入口。

它内部会：

1. 调用 `lona-ir --emit obj`
2. 检查 object bundle 中是否存在 `__lona_main__`
3. 额外生成 hosted entry object
4. 调用系统 linker driver 产出最终程序

### 3.1 最常见用法

```bash
lac input.lo output/program
```

带优化级别：

```bash
lac -O 2 input.lo output/program
```

显式启用 full-LTO：

```bash
lac --lto full -O 3 input.lo output/program
```

指定 hosted target：

```bash
lac --target x86_64-unknown-linux-gnu input.lo output/program
```

链接额外库：

```bash
lac -l m input.lo output/program
lac -L vendor/lib -lsupport input.lo output/program
lac -lsupport input.lo output/program
```

### 3.2 参数

- `-O <0-3>`
  - 转发给 `lona-ir`
- `-I <dir>` / `--include-dir <dir>`
  - 转发给 `lona-ir`，追加模块 root 搜索目录
- `-L <dir>` / `-L<dir>`
  - 追加一个额外库搜索目录到最终 hosted 链接阶段
  - 可重复传入，按出现顺序透传给最终 `cc` / `clang` 链接命令
- `-l <name>` / `-l<name>`
  - 追加一个额外库到最终 hosted 链接阶段
  - 可重复传入，按出现顺序透传给最终 `cc` / `clang` 链接命令
  - 库名解析和搜索规则与 C/C++ 常见 `-L` / `-l` 语义一致
- `--target <triple>`
  - 指定 hosted target
- `--lto <off|full>`
  - 控制是否走 full-LTO 慢路径
- `--cache-dir <dir>`
  - 指定 `lac` 的持久 artifact cache root
  - 默认使用 `${TMPDIR:-/tmp}/lona-cache`
  - hosted 构建会按 `system/<target>/...` 分层缓存模块 object 或 linked-obj bitcode 中间产物
- `--stats`
  - 把 `lona-ir` 的编译统计透传到 stderr
- `--keep-temp`
  - 保留中间 object 和 manifest
- `-h` / `--help`
  - 打印帮助

### 3.3 前置依赖

`lac` 默认需要：

- `lona-ir`
- `cc` 或 `clang`
- `nm`

也可以通过环境变量覆盖：

- `LONA_IR_BIN`
- `LONA_BIN`
- `CC_BIN`
- `NM_BIN`
- `TARGET_TRIPLE`

## 4. `lac-native`

`lac-native` 是 bare native 路径的可执行文件构建入口。

它内部会：

1. 调用 `lona-ir --emit obj`
2. 汇编 bare startup object
3. 用 linker script 和 `ld` 链接出最终 ELF

### 4.1 最常见用法

```bash
lac-native input.lo output/program
```

带优化级别：

```bash
lac-native -O 2 input.lo output/program
```

显式启用 full-LTO：

```bash
lac-native --lto full -O 3 input.lo output/program
```

指定 bare target：

```bash
lac-native --target x86_64-none-elf input.lo output/program
```

### 4.2 参数

- `-O <0-3>`
  - 转发给 `lona-ir`
- `-I <dir>` / `--include-dir <dir>`
  - 转发给 `lona-ir`，追加模块 root 搜索目录
- `--target <triple>`
  - 指定 bare target
- `--lto <off|full>`
  - 控制是否走 full-LTO 慢路径
- `--cache-dir <dir>`
  - 指定 `lac-native` 的持久 artifact cache root
  - 默认使用 `${TMPDIR:-/tmp}/lona-cache`
  - bare 构建会按 `native/<target>/...` 分层缓存模块 object 或 linked-obj bitcode 中间产物
- `--stats`
  - 把 `lona-ir` 的编译统计透传到 stderr
- `--keep-temp`
  - 保留中间 object
- `-h` / `--help`
  - 打印帮助

### 4.3 前置依赖

`lac-native` 默认需要：

- `lona-ir`
- `cc`
- `ld`
- `nm`
- bare startup assembly
- bare linker script

注意：

- `make install` 不会安装 bare runtime 资产
- 使用安装后的 `lac-native` 时，应显式设置：
  - `STARTUP_SRC`
  - `LINKER_SCRIPT`

也可以通过环境变量覆盖：

- `LONA_IR_BIN`
- `LONA_BIN`
- `CC_BIN`
- `LD_BIN`
- `NM_BIN`
- `STARTUP_SRC`
- `LINKER_SCRIPT`
- `TARGET_TRIPLE`

## 5. 建议怎么选

- 想看 AST 或 LLVM IR：用 `lona-ir`
- 想构建普通 Linux hosted 可执行文件：用 `lac`
- 想构建 bare native ELF：用 `lac-native`

如果你只是想“把一个 `.lo` 跑起来”，默认优先走 `lac`。

如果任务涉及 `#[extern "C"]`、bodyless opaque `struct` 或 `#[repr "C"]`，再继续看 [c_ffi.md](c_ffi.md)。
