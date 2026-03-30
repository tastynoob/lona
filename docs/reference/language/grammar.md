# Lona 语言语法文档

> 依据 `grammar/lexer.lex` 与 `grammar/main.yacc` 当前实现整理。本文描述的是解析器现在实际接通的语法入口，以及已经明确保留为占位的能力。

## 示例索引

- 程序结构与顶层项：[program.md](program.md)
- 变量定义：[vardef.md](vardef.md)
- 全局变量：[global.md](global.md)
- 函数声明：[func.md](func.md)
- 结构体声明：[struct.md](struct.md)
- 控制流与块语句：[controlflow.md](controlflow.md)
- 表达式：[expr.md](expr.md)
- 类型写法：[type.md](type.md)
- trait 与 `Trait dyn`：[trait.md](trait.md)

## 1. 词法规则

### 1.1 标识符与字面量

- 标识符 `IDENT`：`[a-zA-Z_][a-zA-Z0-9_]*`
- 整数字面量 `INT`：`[0-9]+`
- 浮点字面量 `FLOAT`：`[0-9]+\.[0-9]+`
- 字符字面量 `CHAR`：`'([^\\']|\\.)*'`
- 字符串字面量 `STRING`：`"[^"]*"`
- 布尔字面量：`true`、`false`
- 空指针字面量：`null`

说明：

- `INT`、`FLOAT`、`CHAR`、`STRING` 在语法层统一归为 `CONST`。
- `CHAR` / `STRING` 当前支持这些转义：`\n`、`\t`、`\r`、`\0`、`\\`、`\'`、`\"`、`\xN`、`\xNN`。
- `null` 是单独关键字，不并入 `CONST`。
- 字面量的具体类型和运行时语义见 [expr.md](expr.md)。

### 1.2 关键字

绑定、语句与声明关键字：

- `var`
- `global`
- `const`
- `def`
- `set`
- `ret`
- `break`
- `continue`
- `if`
- `else`
- `for`
- `struct`
- `trait`
- `impl`
- `import`
- `ref`
- `cast`
- `true`
- `false`
- `null`

类型修饰关键字：

- `const`
- `dyn`

说明：

- `const` 当前同时出现在两个位置：一是前缀只读绑定 `const name ... = expr`，二是 postfix 类型限定 `T const`。

内建类型关键字（词法记号 `BuiltinType`）：

- `type`
- `u8`、`i8`
- `u16`、`i16`
- `u32`、`i32`
- `u64`、`i64`
- `int`、`uint`
- `f32`、`f64`
- `bool`

### 1.3 注释、空白与换行

- 单行注释：`// ...`
- 空格与 Tab 会被忽略。
- 换行具有语法意义，记为 `NL`，主要用作表达式语句和 `ret` 语句的结束标记。
- grammar 会在若干明确位置显式接纳额外的 `NL`，例如 `()` / `[]` 内部、逗号分隔序列里，以及赋值或运算符右侧表达式之前，用来支持多行表达式、多行参数列表和多行类型写法。
- `if expr {` 与 `for expr {` 要求开块 `{` 直接跟在头部表达式后面；这里不能先换行。
- `else` 既可以和前一个 `}` 写在同一行，也可以在若干空行或仅注释行之后起在下一行。
- 但 `else` 自己后面不能换行；当前只接受 `else { ... }` 和 `else if ...`。
- `def name(...) Ret` 如果这一行已经以换行结束，就形成 bodyless function declaration；如果要写函数体，开块 `{` 必须和函数头在同一行。
- `struct Name` 如果这一行已经以换行结束，就形成 opaque struct declaration；如果要写结构体体，开块 `{` 必须和 `struct Name` 在同一行。

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
- 这些符号在表达式层的具体语义见 [expr.md](expr.md)。

## 2. 记号约定

为便于阅读，下面的文法摘要使用以下记号名：

- `IDENT`：普通标识符，对应 `FIELD`
- `BuiltinType`：内建类型关键字，对应 `TYPE`
- `CONST`：整型、浮点、字符串常量
- `BOOL`：`true` 或 `false`
- `NL`：换行

说明：

- `grammar/main.yacc` 中顶层开始符号名写作 `pragram`。本文统一记作 `program`，仅做可读性修正。
- `extern`、`repr` 当前不再是保留关键字；它们作为 tag 名出现在 `#[...]` 里，更具体的 FFI 边界见 [../runtime/c_ffi.md](../runtime/c_ffi.md)。

## 3. 语法摘要

### 3.1 程序结构

```ebnf
program           ::= program-item { program-item }

program-item      ::= NL
                    | stat
                    | import-stat
                    | tagged-global-decl
                    | trait-decl
                    | impl-decl

import-stat       ::= "import" ImportPath NL
```

说明：

- 当前文法不接受完全空文件；至少需要一个顶层项或一个换行。
- `import` 只能放在文件顶层；当前写法是无引号、无后缀的路径，例如 `import math` 或 `import pkg/math`。
- `import` 不属于 `stat`，因此不能出现在块、函数体或结构体体内；写在这些位置会在 parser 阶段报错。
- `global` 也只允许出现在文件顶层，不属于普通 `stat`。
- `trait` 与 `impl Type: Trait` 也只允许出现在文件顶层。

### 3.2 语句

```ebnf
stat              ::= stat-expr
                    | tagged-struct-decl
                    | tagged-func-decl
                    | ret-stat
                    | break-stat
                    | continue-stat
                    | block
                    | if-stat
                    | for-stat

block             ::= "{"
                      { NL | stat }
                      "}"

if-stat           ::= "if" expr block
                    | "if" expr block "else" block
                    | "if" expr block "else" if-stat

for-stat          ::= "for" expr block
                    | "for" expr block "else" block

ret-stat          ::= "ret" NL
                    | "ret" expr NL

break-stat        ::= "break" NL

continue-stat     ::= "continue" NL

stat-expr         ::= final-expr NL
                    | tagged-var-def NL

tagged-var-def    ::= var-def
                    | tag-line var-def
```

说明：

- 控制流执行语义见 [controlflow.md](controlflow.md)。

### 3.3 结构体、trait、impl、函数与全局声明

```ebnf
tag-line          ::= "#" "[" tag-entry-seq "]" NL

tag-entry-seq     ::= tag-entry
                    | tag-entry-seq "," { NL } tag-entry

tag-entry         ::= IDENT
                    | IDENT tag-arg-seq

tag-arg-seq       ::= tag-arg
                    | tag-arg-seq tag-arg

tag-arg           ::= IDENT
                    | BuiltinType
                    | CONST

tagged-struct-decl ::= struct-decl
                     | tag-line struct-decl

struct-decl       ::= "struct" IDENT NL
                    | "struct" IDENT "{" "}"
                    | "struct" IDENT "{"
                      ( struct-stat | NL )
                      { NL | struct-stat }
                      "}"

struct-stat       ::= field-decl NL
                    | tagged-func-decl

tagged-func-decl  ::= func-decl
                    | tag-line func-decl

tagged-global-decl ::= global-decl
                     | tag-line global-decl

global-decl       ::= "global" IDENT type-name NL
                    | "global" IDENT type-name "=" expr NL
                    | "global" IDENT type-name "=" brace-init NL
                    | "global" IDENT "=" expr NL
                    | "global" IDENT "=" brace-init NL

func-decl         ::= [ "set" ] "def" IDENT "(" ")" NL
                    | [ "set" ] "def" IDENT "(" ")" type-name NL
                    | [ "set" ] "def" IDENT "(" param-decl-seq ")" NL
                    | [ "set" ] "def" IDENT "(" param-decl-seq ")" type-name NL
                    | [ "set" ] "def" IDENT "(" ")" block
                    | [ "set" ] "def" IDENT "(" ")" type-name block
                    | [ "set" ] "def" IDENT "(" param-decl-seq ")" block
                    | [ "set" ] "def" IDENT "(" param-decl-seq ")" type-name block

trait-decl        ::= "trait" IDENT NL
                    | "trait" IDENT "{ }"
                    | "trait" IDENT "{"
                      ( trait-stat | NL )
                      { NL | trait-stat }
                      "}"

trait-stat        ::= trait-func-decl
                    | /* parser 还会接纳更多成员与语句形状，语义阶段再给 targeted diagnostic */

trait-func-decl   ::= [ "set" ] "def" IDENT "(" ")" NL
                    | [ "set" ] "def" IDENT "(" ")" type-name NL
                    | [ "set" ] "def" IDENT "(" param-decl-seq ")" NL
                    | [ "set" ] "def" IDENT "(" param-decl-seq ")" type-name NL

impl-decl         ::= "impl" type-name ":" dot-like-name NL
                    | "impl" type-name ":" NL* dot-like-name NL
                    | "impl" type-name ":" dot-like-name block
                    | "impl" type-name ":" NL* dot-like-name block

field-decl        ::= IDENT type-name
                    | "_" type-name
                    | "set" IDENT type-name

var-decl          ::= IDENT type-name
const-var-def     ::= "const" IDENT "=" expr
                    | "const" IDENT "=" brace-init
                    | "const" var-decl "=" expr
                    | "const" var-decl "=" brace-init
param-decl        ::= IDENT type-name
                    | "ref" IDENT type-name

param-decl-seq    ::= param-decl
                    | param-decl-seq "," param-decl
```

说明：

- tag line 必须单独占一行，然后紧跟一个函数声明、结构体声明或变量定义。
- tag line 也可以跟一个 `global` 声明。
- 当前内建 tag 只有 `extern` 和 `repr`。
- `#[extern "C"]` 只接受一个字符串参数 `"C"`，当前用于 C ABI 顶层函数。
- `#[extern] global name T` 用于外部全局符号声明；它不接受参数。
- `#[extern] struct Name` 已移除；opaque 类型统一写成 bodyless `struct Name`。
- `#[repr "C"]` 只接受一个字符串参数 `"C"`，当前用于 C-compatible 结构体。
- 普通 `global` 当前必须带初始化器；`global name T` 只对 `#[extern] global` 有意义。
- `const name = expr` / `const name T = expr` 是变量定义语法，不是类型后缀；它会先按普通变量那样做推断或类型检查，再在最外层补一层 `const`。
- `param-decl-seq` 当前不支持尾逗号；例如 `def sum(a i32, b i32,)` 会在 parser 阶段报错。
- `struct Name` 与后面的 `{` 必须写在同一行；`struct Name` 单独占一行时表示 opaque struct declaration。
- `def name(...) Ret` 与后面的 `{` 也必须写在同一行；如果头部已经以换行结束，parser 会把它视为函数声明。
- `trait Name` 与后面的 `{` 也必须写在同一行；`trait Name` 单独占一行时表示空 trait declaration。
- `trait` body 当前稳定语义只接受方法签名；为了给用户更明确的 targeted diagnostic，parser 还会暂时接纳 `field`、`var`、`global`、`ret`、`if`、`for`、块语句等形状，然后在语义阶段统一拒绝。
- `impl Type: Trait` 当前只稳定支持 header；`impl ... { ... }` 这条语法入口保留是为了发出“trait impl body 尚未支持”的定向诊断。
- 结构体、顶层函数和 C FFI tag 的语义分别见 [struct.md](./struct.md)、[func.md](./func.md) 和 [../runtime/c_ffi.md](../runtime/c_ffi.md)。
- `global` 的运行时语义与当前初始化限制见 [global.md](./global.md)。
- trait / impl / `Trait dyn` 的完整语义见 [trait.md](./trait.md)。

### 3.4 变量定义

```ebnf
var-def           ::= "var" var-decl
                    | "var" var-decl "=" expr
                    | "var" var-decl "=" array-init
                    | "const" IDENT "=" expr
                    | "const" IDENT "=" array-init
                    | "const" var-decl "=" expr
                    | "const" var-decl "=" array-init
                    | "ref" IDENT type-name "=" expr
                    | "ref" IDENT type-name "=" array-init
                    | "var" IDENT "=" expr
                    | "var" IDENT "=" array-init
                    | IDENT ":" "=" expr
                    | IDENT ":" "=" array-init
```

说明：

- 当前 array init 先收口为“初始化语法”，而不是任意位置都可用的裸表达式。
- `var` 显式类型绑定只接受可变存储；像 `var x i32 const = 1`、`var p T* const = ...` 这类最外层 `const` 写法会在语义阶段报错。只读绑定改用 `const name = expr` 或 `const name T = expr`。
- 变量定义、推断和初始化语义见 [vardef.md](./vardef.md)。

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
                    | field-call

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

cast-expr         ::= "cast" "[" type-name "]" "(" expr ")"

tuple-literal     ::= "(" expr "," expr-seq ")"

func-pointer-ref  ::= IDENT "&<" ">"
                    | IDENT "&<" func-param-type-seq ">"

single-value      ::= variable
                    | CONST
                    | BOOL
                    | cast-expr
                    | func-pointer-ref
                    | field-call
                    | expr-paren
                    | tuple-literal

field-call        ::= single-value "(" ")"
                    | single-value "(" call-arg-seq ")"

variable          ::= IDENT
                    | field-selector

field-selector    ::= single-value "." IDENT

func-param-type   ::= type-name
                    | "ref" type-name

func-param-type-seq ::= func-param-type
                      | func-param-type-seq "," func-param-type

expr-seq          ::= expr
                    | expr-seq "," expr

call-arg-seq      ::= call-arg
                    | call-arg-seq "," NL* call-arg

call-arg          ::= expr
                    | "ref" expr
                    | brace-init
                    | named-call-arg

named-call-arg    ::= IDENT "=" expr
                    | "ref" IDENT "=" expr
                    | IDENT "=" brace-init

brace-init        ::= "{ }"
                    | "{"
                      NL*
                      "}"
                    | "{"
                      brace-inline-body
                      [ "," NL* ]
                      "}"
                    | "{"
                      NL
                      brace-line-body
                      "}"

brace-inline-body ::= brace-init-item
                    | brace-inline-body "," NL* brace-init-item

brace-line-body   ::= NL* brace-line-seq NL*

brace-line-seq    ::= brace-line-entry
                    | brace-line-seq brace-line-entry

brace-line-entry  ::= brace-init-item [ "," ] NL NL*

brace-init-item   ::= expr
                    | brace-init
```

说明：

- 当前 `xxx(...)` 统一视为“括号应用”语法；语义阶段再区分它是函数调用、函数指针调用还是数组索引。
- `expr-assign-left` 在 parser 层包含 `field-call`，因此 `a(1)`、`grid(1, 2)` 这类数组索引写法可以出现在赋值左侧。
- 如果形参是 `ref`，调用点也必须显式写 `ref`，例如 `inc(ref x)`、`inc(ref value = x)`；结构体方法继续使用普通成员调用语法，具体规则见 [struct.md](./struct.md)。
- `_ T` 是嵌入字段声明。它在语义上仍然是一个真实结构体字段，只是源码里不直接写字段名。
- 当前实现不支持 `set _ T`；如果写出它，语义阶段会报错。
- selector lookup 会把唯一的嵌入路径视为 promoted member，因此 `obj.name` 既可能命中直接成员，也可能命中某条嵌入路径上的成员。
- 位置实参允许出现在命名实参前面，例如 `mix(1, y=2)`；命名实参后面不能再跟位置实参。
- 普通 `()` / `[]` 里的逗号序列当前都不支持尾逗号；例如 `foo(1, 2,)`、`(1, 2,)`、`i32[4,]` 都会在 parser 阶段报错。
- 花括号初始化当前用于数组。
- 只有花括号初始化列表支持尾逗号；例如 `{1, 2,}` 与多行 `{\n  1,\n  2,\n}` 都合法。
- 多行花括号初始化当前同时接受两种风格：逐行分组 `{\n  1\n  2\n}`，以及逗号分隔风格 `{\n  1,\n  2,\n}`。
- 表达式、结构体类型名的 call-like 初始化、数组初始化和 `ref` 的具体语义见 [expr.md](./expr.md)、[vardef.md](./vardef.md)、[type.md](./type.md)、[pointer.md](./pointer.md) 和 [ref.md](./ref.md)。

### 3.6 类型语法

```ebnf
type-name         ::= postfix-type

postfix-type      ::= type-primary
                    | postfix-type "*"
                    | postfix-type "[" "*" "]"
                    | postfix-type "[" "]"
                    | postfix-type "[" expr-seq "]"
                    | postfix-type "[" "," expr-seq "]"
                    | postfix-type "dyn"
                    | postfix-type "const"

type-primary      ::= single-type
                    | tuple-type
                    | func-type

single-type       ::= IDENT
                    | BuiltinType
                    | type-selector

tuple-type        ::= "<" type-name-seq ">"

func-type         ::= "(" ":" ")"
                    | "(" ":" type-name ")"
                    | "(" type-name-seq ":" ")"
                    | "(" type-name-seq ":" type-name ")"

type-selector     ::= IDENT "." IDENT
                    | type-selector "." IDENT

type-name-seq     ::= type-name
                    | type-name-seq "," type-name
```

说明：

- 元组类型 `<T1, T2, ...>` 已经进入 parser / AST。
- 顶层函数声明仍写成 `def foo(v i32) i32` 这种“参数头 + 返回类型”形式。
- 但在类型位置里，parser 只接受显式函数指针，例如 `(:)`、`(i32, bool: i32)`。
- 裸函数签名如 `(i32, bool) i32` 不再作为 `type-name` 的合法写法。
- 函数指针类型本身是一个完整的 `type-primary`，因此后面可以继续接普通后缀，例如 `(i32: i32)*`、`(: i32)[4]`。
- 函数取指针不在类型层完成，而是通过表达式 `foo&<i32, bool>` 显式写出。
- 连续 `[]` 和单个 `[,]` 当前都已进入类型语法。
- `postfix-type "dyn"` 当前只接受 trait 名，因此用户层稳定写法是 `Hash dyn`、`dep.Hash dyn`。
- `base-type "[*]"` 现在表示稳定可用的“可索引指针”类型。
- `base-type "[]"` 这种显式未定长数组类型写法对用户是禁止的；如果想省略数组维度，请用 `var a = {1, 2}` 这类初始化器推断。
- 更具体的类型语义见 [type.md](./type.md)。

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
- 表达式层的数组访问统一走 `()`，并在语义阶段区分为函数调用或数组索引。
- 运算符的具体语义见 [expr.md](./expr.md)。

## 5. 当前实现边界

### 5.1 仍保留为占位的能力

- 未定长数组语义 (`T[]`)

这些路径当前不会再落入模糊的 generic unsupported，而是给出明确的面向用户的占位诊断。
