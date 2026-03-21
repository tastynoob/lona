# 程序结构示例

> 对应 `docs/grammer.md` 的“3.1 程序结构”和“3.2 语句”。

## 1. 顶层可以直接放语句

```lona
var answer = 42
answer += 1
```

## 2. 顶层可以放函数声明

```lona
def id(x i32) i32 {
    ret x
}
```

## 3. 顶层可以放结构体声明

```lona
struct Point {
    x i32
    y i32
}
```

## 4. 顶层允许空行

```lona

var x = 1

def inc(v i32) i32 {
    ret v + 1
}
```

## 5. 块语句可以包含多条语句

```lona
{
    var x = 1
    x += 2
    ret x
}
```

## 6. 空块是合法的

```lona
def noop() {}
```

## 7. 顶层可以导入其他源文件

```lona
import math

def main() i32 {
    ret math.add_one(41)
}
```

说明：

- `import` 只能出现在文件顶层。
- 如果把 `import` 写进块、函数体或结构体体内，当前 parser 会直接报错。
- 导入路径写成无引号、无文件后缀的形式；实际会按 `.lo` 文件解析。
- imported module 的顶层函数通过 `file.xxx(...)` 的形式访问，模块名来自被导入文件的文件名。
- 导入路径按当前文件所在目录解析。
