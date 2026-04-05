# 程序结构示例

> 对应 `grammar.md` 的“3.1 程序结构”和“3.2 语句”。

## 1. 顶层可以声明全局变量

```lona
global counter = 1
global total i64 = 42_i64
```

`global` 的完整规则见 [global.md](global.md)。

## 2. 顶层可以直接放语句

```lona
var answer = 42
answer += 1
```

## 3. 顶层可以放函数声明

```lona
def id(x i32) i32 {
    ret x
}
```

## 4. 顶层可以放结构体声明

```lona
struct Point {
    x i32
    y i32
}
```

## 5. 顶层可以放 trait 声明

```lona
trait Hash {
    def hash() i32
}
```

trait 的完整规则见 [trait.md](trait.md)。

## 6. 顶层可以放 impl 声明

```lona
impl Point: Hash
impl Point: dep.Hash
impl Hash for Point {
    def hash() i32 {
        ret self.value + 1
    }
}
```

说明：

- `impl Type: Trait` 和 `impl Trait for Type` 都是顶层声明。
- `impl Trait for Type { ... }` 当前支持最小可用的 impl body。
- 这版 impl body 只稳定支持 local、non-generic、concrete struct self type。
- `obj.method()`、`Trait.method(&obj)`、`obj.Trait.method()` 和 `Trait dyn` 都可以调用这些 impl body 里定义的方法。
- trait 方法和普通成员方法现在属于不同命名空间；如果有普通成员方法同名，`obj.method()` 仍然优先命中普通成员方法。
- 当同一类型上有多个 trait 提供同名方法时，普通 `obj.method()` 会报歧义，此时改写成 `obj.Trait.method()` 或 `Trait.method(&obj)`。

## 7. 顶层允许空行

```lona

var x = 1

def inc(v i32) i32 {
    ret v + 1
}
```

## 8. 块语句可以包含多条语句

```lona
{
    var x = 1
    x += 2
    ret x
}
```

## 9. 空块是合法的

```lona
def noop() {}
```

## 10. 顶层可以导入其他源文件

```lona
import math

var answer = math.add_one(41)
ret answer
```

说明：

- `import` 只能出现在文件顶层。
- 如果把 `import` 写进块、函数体或结构体体内，当前 parser 会直接报错。
- 导入路径写成无引号、无文件后缀的形式；实际会按 `.lo` 文件解析。
- imported module 的顶层函数通过 `file.xxx(...)` 的形式访问，模块名来自被导入文件的文件名。
- 相对导入路径会先按当前文件所在目录解析，再按命令行传入的 `-I` / `--include-dir` 搜索目录继续查找。
- 绝对导入路径会直接按绝对路径解析。
- 被导入文件的 base name 必须是合法标识符；否则它不能稳定映射成 `file.xxx` 形式的模块名。

## 11. imported 模块也可以包含顶层执行语句

```lona
// dep.lo
global count = 0

count = count + 1
```

说明：

- imported 模块可以像 root 模块一样混合 `import`、`struct`、`def`、`global` 和顶层执行语句。
- 当某个模块第一次被执行到时，编译器会先初始化它直接依赖的模块，再执行它自己的顶层语句。
- 同一个模块的顶层执行体只会运行一次；多个 importer 不会重复初始化同一个模块。
- imported 模块顶层如果执行 `ret <nonzero>`，这个返回值会作为模块初始化失败码继续向上传播。

## 12. root 模块的可执行入口

root 模块允许直接写顶层可执行语句：

```lona
var answer = 41
answer = answer + 1
```

规则是：

- root 模块仍然保留语言入口 `__lona_main__() -> i32` 这个特殊规则。
- imported 模块的顶层执行语句会被 lower 到各自模块的内部初始化入口，再由 importer 链式触发。
- `__lona_main__()` 进入 root 模块后，也会先触发依赖模块初始化，再执行 root 自己的顶层语句。
- root 模块顶层 `ret <code>` 仍然会成为 `__lona_main__()` 的返回值，也就是当前 native 可执行文件的退出码。
- 如果程序没有建立 `__lona_main__()`，可执行文件构建路径会报缺少入口。
- `def main() i32` 现在只是普通函数名，不再自动提升成程序入口。
- 宿主系统的 `main(argc, argv)` wrapper、bare `_start` 和可执行文件构建路径见 [../runtime/native_build.md](../runtime/native_build.md)。
