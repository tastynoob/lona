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

## 5. 顶层允许空行

```lona

var x = 1

def inc(v i32) i32 {
    ret v + 1
}
```

## 6. 块语句可以包含多条语句

```lona
{
    var x = 1
    x += 2
    ret x
}
```

## 7. 空块是合法的

```lona
def noop() {}
```

## 8. 顶层可以导入其他源文件

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
- 导入路径按当前文件所在目录解析。

## 9. imported 模块可以包含声明，但不能包含顶层执行语句

```lona
// dep.lo
global count = 1

def bump() i32 {
    count = count + 1
    ret count
}
```

说明：

- imported 模块可以包含 `import`、`struct`、`def`、`global` 这些顶层声明。
- imported 模块仍然不能包含顶层可执行语句。

## 10. root 模块的可执行入口

root 模块允许直接写顶层可执行语句：

```lona
var answer = 41
answer = answer + 1
```

规则是：

- 只有 root 模块允许顶层可执行语句。
- imported 模块仍然只能包含声明，不能包含顶层执行语句。
- root 模块如果存在顶层可执行语句，语言内部会建立 `__lona_main__() -> i32` 作为入口。
- `def main() i32` 现在只是普通函数名，不再自动提升成程序入口。
- 宿主系统的 `main(argc, argv)` wrapper、bare `_start` 和可执行文件构建路径见 [../runtime/native_build.md](../runtime/native_build.md)。
