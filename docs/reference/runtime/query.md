# `lona-query` 使用

`lona-query` 是面向静态分析器、编辑器插件和后续 LSP 外壳的查询前端。

它复用当前 `lona` 编译器前端的 parser、模块加载、`resolve` 和 `analysis`，但不进入 LLVM codegen。它的目标不是替代 `lona-ir`，而是提供一个稳定、可脚本化的源码查询入口。

## 1. 适用场景

- 查询当前项目的诊断、AST、顶层声明和局部变量
- 给外层 TypeScript / LSP 进程提供子进程查询接口
- 在源码变更后，通过 `reload` 验证模块和依赖模块是否已经更新

不适合用它做的事情：

- 产出 LLVM IR、object 或最终可执行文件
- 替代 `lac` / `lac-native`
- 充当完整 LSP 协议服务端

## 2. 构建与启动

安装后直接调用：

```bash
lona-query src third_party/modules
```

从源码树内使用：

```bash
make query
./build/lona-query src
./build/lona-query src third_party/modules vendor/modules
```

直接加载内存源码：

```bash
./build/lona-query --source 'func main() { ret 0 }' --path demo.lo
```

单次 JSON 查询：

```bash
./build/lona-query --format json --command status src third_party/modules
```

启动参数：

- `--source <text>`
  - 直接加载一段源码文本
- `--path <path>`
  - 与 `--source` 配合使用，指定虚拟源文件路径
- `--error-limit <n>`
  - 控制本轮最多收集多少条诊断
- `--format text|json`
  - 控制输出格式
- `--command <command>`
  - 执行一条命令后退出

位置参数：

- `<root path...>`
  - 初始 root paths，语义等同于后续执行一次 `root <path...>`

## 3. 文本模式与 JSON 模式

文本模式默认进入 REPL，并输出 `lona-query>` prompt。

```bash
./build/lona-query src third_party/modules
```

JSON 模式适合给外部程序直接消费：

- 每条响应都是单行 JSON
- 不输出 prompt
- 不输出启动摘要
- 不带 `--command` 时，进程会从标准输入持续读取命令

例如：

```bash
printf 'status\nquit\n' | ./build/lona-query --format json src third_party/modules
```

响应统一形状为：

```json
{"ok":true,"command":"status","result":{...}}
```

命令执行失败时：

- `ok` 为 `false`
- 错误文本放在 `result.error`

## 4. Root 与重载模型

`lona-query` 按“root paths + 已加载 entry modules”组织查询状态。

- `root <path...>`
  - 设置当前会话的 root paths
  - 这一步不会自动打开任何模块
- `gotom <module>`
  - 切换当前活动模块
  - 如果模块尚未加载，会按 root paths 解析并增量加载该模块及其依赖闭包
  - 这里的 `<module>` 必须使用 canonical 模块路径，也就是“相对于 root paths 的路径，去掉 `.lo` 后缀”
- `reload`
  - 重新加载当前已加载的 entry modules 及其依赖图
- `reload <module>`
  - 重新加载指定模块，并失效当前已加载图里依赖它的模块
  - 这里的 `<module>` 必须使用 canonical 模块路径，也就是“相对于 root paths 的路径，去掉 `.lo` 后缀”

边界：

- `gotom <module>` 只适用于 file-backed root-path 会话
- `gotom <module>` 不接受绝对路径
- `gotom <module>` 不接受 `.lo` 后缀
- `gotom <module>` 如果在多个 root paths 下同时命中，会直接报冲突，而不是按顺序挑一个
- `reload <module>` 只适用于 file-backed root-path 会话
- `reload <module>` 不接受绝对路径
- `reload <module>` 不接受 `.lo` 后缀
- `reload <module>` 如果在多个 root paths 下同时命中，会直接报冲突，而不是按顺序挑一个
- 如果当前是 `--source` 内存源码模式，只能使用不带参数的 `reload`
- 当前支持的是模块级重载，还不支持函数级局部重载
- 如果一个模块还没被 `gotom` 打开过，它不属于当前已加载集合；这时它的诊断也不会自动出现

## 5. 当前命令

- `help`
  - 打印命令列表
- `status`
  - 打印当前会话状态
- `root <path...>`
  - 设置 root paths
- `gotom <module>`
  - 切换当前活动模块
- `reload [module]`
  - 重新加载当前已加载集合，或重新加载一个 canonical 模块路径及其依赖方
- `goto <line>`
  - 把当前分析点移动到某一行
- `diagnostics`
  - 打印当前收集到的诊断
- `info global`
  - 打印当前项目里已索引的非局部符号
- `info local [line]`
  - 打印当前行可见的局部变量、参数和 `self`
- `ast`
  - 打印当前活动模块的 AST JSON
- `print <name>`
  - 精确打印一个符号或字段
- `find [kind] [pattern]`
  - 模糊搜索已索引符号
- `quit`
  - 退出会话

`find` 目前支持的 kind 过滤包括：

- `type`
- `trait`
- `impl`
- `func`
- `method`
- `field`
- `global`
- `import`
- `all`

## 6. `print` 与 `find`

`print` 适合做精确查询，`find` 适合做模糊搜索。

例如：

```text
root src third_party/modules
gotom main
print Complex
print Trait
print value
gotom helper
goto 42
print self
print local_name
find field value
find func fib
```

`print <name>` 当前会按下面的顺序尝试解析：

1. 当前作用域可见的局部变量、参数、`self`
2. 顶层变量 / 全局定义
3. 顶层 type / trait / func
4. 字段

如果名称有歧义，应当使用更明确的查询上下文，或者先用 `find` 缩小范围。

## 7. 当前边界

`lona-query` 目前已经复用了编译器前端的大部分语义状态，但仍有明确边界：

- 现在是“查询前端”，不是 LSP server
- 现在支持模块级 `reload`，还不支持函数体级增量失效
- 现在的诊断语义是“当前已加载 entry modules 的诊断”，不是“整个 root paths 下所有潜在目标的完整诊断”
- JSON 输出适合机器消费，但不是正式的 LSP 协议消息
- 命令和 JSON 字段会继续演进，外层包装最好只依赖已经文档化的字段和命令
