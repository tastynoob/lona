# Lona 语言语法文档

> 依据 `grammar/lexer.lex` 与 `build/gen.yacc` 当前实现整理。本文描述的是解析器现在实际接通的语法入口，以及已经明确保留为占位的能力。

## 示例索引

- 程序结构与顶层项：[program.md](program.md)
- 变量定义：[vardef.md](vardef.md)
- 函数声明：[func.md](func.md)
- 结构体声明：[struct.md](struct.md)
- 控制流与块语句：[controlflow.md](controlflow.md)
- 表达式：[expr.md](expr.md)
- 类型写法：[type.md](type.md)

## 1. 词法规则

### 1.1 标识符与字面量

- 标识符 `IDENT`：`[a-zA-Z_][a-zA-Z0-9_]*`
- 整数字面量 `INT`：`[0-9]+`
- 浮点字面量 `FLOAT`：`[0-9]+\.[0-9]+`
- 字符串字面量 `STRING`：`"[^"]*"`
- 布尔字面量：`true`、`false`

说明：

- `INT`、`FLOAT`、`STRING` 在语法层统一归为 `CONST`。
- `FLOAT` 当前已经形成最小浮点语义闭环；`STRING` 仍保留为占位。

### 1.2 关键字

语句与声明关键字：

- `var`
- `def`
- `ret`
- `if`
- `else`
- `for`
- `struct`
- `import`

内建类型关键字（词法记号 `BuiltinType`）：

- `type`
- `u8`、`i8`
- `u16`、`i16`
- `u32`、`i32`
- `u64`、`i64`
- `int`、`uint`
- `f32`、`f64`
- `bool`
- `str`

### 1.3 注释、空白与换行

- 单行注释：`// ...`
- 空格与 Tab 会被忽略。
- 换行具有语法意义，记为 `NL`，主要用作表达式语句和 `ret` 语句的结束标记。

### 1.4 词法器可识别的符号

```text
+  -  *  /  %  !  ~  <  >  |  &  ^
+= -= *= /= %= &= ^= |= <<= >>= == != <= >= << >> && ||
{ } : = ( ) [ ] @ # , .
&<
```

说明：

- `&<` 是显式函数取指针的专用起始符。
- 词法层已经把常见 C 风格运算符家族接了进来。
- 当前 grammar 里列出的常见一元 / 二元 / 复合赋值运算符，已经都接通了 built-in 语义。

## 2. 记号约定

为便于阅读，下面的文法摘要使用以下记号名：

- `IDENT`：普通标识符，对应 `FIELD`
- `BuiltinType`：内建类型关键字，对应 `TYPE`
- `CONST`：整型、浮点、字符串常量
- `BOOL`：`true` 或 `false`
- `NL`：换行

说明：

- `build/gen.yacc` 中顶层开始符号名写作 `pragram`。本文统一记作 `program`，仅做可读性修正。

## 3. 语法摘要

### 3.1 程序结构

```ebnf
program           ::= program-item { program-item }

program-item      ::= NL
                    | stat
                    | import-stat

import-stat       ::= "import" ImportPath NL
```

说明：

- 当前文法不接受完全空文件；至少需要一个顶层项或一个换行。
- `import` 只能放在顶层；当前写法是无引号、无后缀的路径，例如 `import math` 或 `import pkg/math`。
- 导入文件当前只允许声明，不允许顶层可执行语句。

### 3.2 语句

```ebnf
stat              ::= stat-expr
                    | struct-decl
                    | func-decl
                    | ret-stat
                    | block
                    | if-stat
                    | for-stat

block             ::= "{"
                      { NL | stat }
                      "}"

if-stat           ::= "if" expr block
                    | "if" expr block "else" block

for-stat          ::= "for" expr block

ret-stat          ::= "ret" NL
                    | "ret" expr NL

stat-expr         ::= final-expr NL
                    | var-def NL
```

### 3.3 结构体与函数声明

```ebnf
struct-decl       ::= "struct" IDENT "{"
                      struct-stat
                      { struct-stat }
                      "}"

struct-stat       ::= var-decl NL
                    | func-decl

func-decl         ::= "def" IDENT "(" ")" block
                    | "def" IDENT "(" ")" type-name block
                    | "def" IDENT "(" var-decl-seq ")" block
                    | "def" IDENT "(" var-decl-seq ")" type-name block

var-decl          ::= IDENT type-name
var-decl-seq      ::= var-decl
                    | var-decl-seq "," var-decl
```

### 3.4 变量定义

```ebnf
var-def           ::= "var" var-decl
                    | "var" var-decl "=" expr
                    | "var" var-decl "=" array-init
                    | "var" IDENT "=" expr
                    | "var" IDENT "=" array-init
                    | IDENT ":" "=" expr
                    | IDENT ":" "=" array-init
```

说明：

- 当前 array init 先收口为“初始化语法”，而不是任意位置都可用的裸表达式。

### 3.5 表达式

```ebnf
final-expr        ::= expr
                    | expr-assign

expr              ::= expr-binop
                    | expr-unary
                    | single-value

expr-assign       ::= expr-assign-left "=" expr
                    | expr-assign-left "+=" expr
                    | expr-assign-left "-=" expr
                    | expr-assign-left "*=" expr
                    | expr-assign-left "/=" expr
                    | expr-assign-left "%=" expr
                    | expr-assign-left "&=" expr
                    | expr-assign-left "^=" expr
                    | expr-assign-left "|=" expr
                    | expr-assign-left "<<=" expr
                    | expr-assign-left ">>=" expr

expr-assign-left  ::= variable
                    | expr-getpointee

expr-binop        ::= expr "*" expr
                    | expr "/" expr
                    | expr "%" expr
                    | expr "+" expr
                    | expr "-" expr
                    | expr "<<" expr
                    | expr ">>" expr
                    | expr "<" expr
                    | expr ">" expr
                    | expr "<=" expr
                    | expr ">=" expr
                    | expr "==" expr
                    | expr "!=" expr
                    | expr "&" expr
                    | expr "^" expr
                    | expr "|" expr
                    | expr "&&" expr
                    | expr "||" expr

expr-unary        ::= "!" single-value
                    | "~" single-value
                    | "+" single-value
                    | "-" single-value
                    | "&" single-value
                    | expr-getpointee

expr-getpointee   ::= "*" single-value

expr-paren        ::= "(" expr ")"

legacy-cast-expr  ::= BuiltinType IDENT
                    | BuiltinType CONST
                    | BuiltinType BOOL
                    | BuiltinType expr-paren

tuple-literal     ::= "(" expr "," expr-seq ")"

func-pointer-ref  ::= IDENT "&<" ">"
                    | IDENT "&<" type-name-seq ">"

single-value      ::= variable
                    | CONST
                    | BOOL
                    | legacy-cast-expr
                    | func-pointer-ref
                    | field-call
                    | expr-paren
                    | tuple-literal

field-call        ::= single-value "(" ")"
                    | single-value "(" expr-seq ")"

variable          ::= IDENT
                    | field-selector

field-selector    ::= single-value "." IDENT

expr-seq          ::= expr
                    | expr-seq "," expr

array-init        ::= "{ }"
                    | "{"
                      array-init-seq
                      "}"

array-init-seq    ::= expr
                    | array-init
                    | array-init-seq "," expr
                    | array-init-seq "," array-init
```

说明：

- `legacy-cast-expr` 不是正式特性，而是为了给旧式 `i32 value` / `i32(expr)` 写法提供明确错误诊断。
- 当前 `xxx(...)` 统一视为“括号应用”语法；具体是函数调用、函数指针调用，还是未来的数组访问 / 其它重载行为，由后续语义阶段决定。
- 当前 `aaa.bbb(...)` 除了结构体方法，也可以命中“成员函数注入”入口；内建数值转换 `aaa.tof32()` / `aaa.toi32()` 和位模式视图 `aaa.tobits()` 都走这条路径，但后端会直接 lower 成高效 cast / byte-copy，不生成真实函数调用。
- 元组字面量当前已经支持“显式 tuple 目标类型 + 构造/传递”这一最小闭环。
- 元组成员访问沿用 `field-selector` 规则，字段名按 `_1`、`_2`、`_3` 这种自动生成名称访问。
- 固定维度数组现在已经支持零初始化占位与 `()` 索引 lowering。
- 显式数组元素列表仍未实现；当前只接受零初始化占位。

### 3.6 类型语法

```ebnf
type-name         ::= base-type
                    | func-head type-name

base-type         ::= single-type
                    | tuple-type
                    | ptr-type
                    | array-type

single-type       ::= IDENT
                    | BuiltinType
                    | type-selector

tuple-type        ::= "<" type-name-seq ">"

type-selector     ::= IDENT "." IDENT
                    | type-selector "." IDENT

ptr-type          ::= base-type "*"

array-type        ::= base-type "[" "]"
                    | base-type "[" expr-seq "]"
                    | base-type "[" "," expr-seq "]"

func-head         ::= "(" ")"
                    | "(" type-name-seq ")"
                    | func-head "*"
                    | func-head "[" "]"
                    | func-head "[" expr-seq "]"
                    | func-head "[" "," expr-seq "]"

type-name-seq     ::= type-name
                    | type-name-seq "," type-name
```

说明：

- 元组类型 `<T1, T2, ...>` 已经进入 parser / AST。
- 函数类型必须写成“参数头 + 返回类型”，例如 `(i32, bool) i32`。
- 函数取指针不在类型层完成，而是通过表达式 `foo&<i32, bool>` 显式写出。
- 连续 `[]` 和单个 `[,]` 当前都已进入类型语法，但它们在语义上表示不同的容器组合方式。

## 4. 运算符优先级与结合性

从低到高如下：

| 优先级 | 运算符 | 结合性 |
| --- | --- | --- |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `^=` `|=` `<<=` `>>=` | 右结合 |
| 2 | `||` | 左结合 |
| 3 | `&&` | 左结合 |
| 4 | `|` | 左结合 |
| 5 | `^` | 左结合 |
| 6 | `&` | 左结合 |
| 7 | `==` `!=` | 左结合 |
| 8 | `<` `>` `<=` `>=` | 左结合 |
| 9 | `<<` `>>` | 左结合 |
| 10 | `+` `-` | 左结合 |
| 11 | `*` `/` `%` | 左结合 |
| 12 | `.` | 左结合 |
| 13 | `()` `[]` | 左结合 |
| 14 | 一元 `!` `~` `+` `-` `&` `*` | 右结合 |

说明：

- 上表描述的是 parser 当前的优先级表。
- 当前 grammar 里列出的常见运算符已经形成完整 built-in 语义；后续如需扩展重载，会继续沿用语义阶段的运算符解析层。
- 表达式层的数组访问统一走 `()`，并在语义阶段区分为函数调用或数组索引。

## 5. 当前实现边界

### 5.1 仍保留为占位的能力

- explicit array element initializer
- array dimension inference
- string runtime semantics

这些路径当前不会再落入模糊的 generic unsupported，而是给出明确的面向用户的占位诊断。

### 5.2 函数指针可以间接调用

当前可以通过 `foo&<i32>` 这样的形式显式取得函数指针，并像普通可调用值一样继续调用。

例如下面这些形式都可以工作：

```text
var cb = foo&<i32>
cb(1)

make_cb()(1)
box.callback(1)
```

其中 `box.callback(1)` 这里的 `callback` 是结构体字段里的函数指针，不是方法选择器；真正的方法选择器仍然要求直接以 `obj.method(...)` 的形式调用。

### 5.3 函数相关存储需要定义时初始化

当前实现继续禁止裸函数类型变量；此外，函数指针或函数相关数组这类“包裹了函数类型”的变量存储，也要求在 `var` 定义时立刻给出初始化值，避免未初始化的野函数指针。

例如下面这种写法会报错：

```text
var cb (i32)* i32
```
