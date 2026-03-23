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
- 空指针字面量：`null`

说明：

- `INT`、`FLOAT`、`STRING` 在语法层统一归为 `CONST`。
- `FLOAT` 当前已经形成最小浮点语义闭环。
- `STRING` 在语义层按 byte string literal 处理；`"..."` 是 `u8 const[N]`，`&"..."` 是 `u8 const[*]`。
- `null` 是单独关键字，不并入 `CONST`；语义层只允许它进入指针上下文。

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
- `null`

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
- `import` 只能放在文件顶层；当前写法是无引号、无后缀的路径，例如 `import math` 或 `import pkg/math`。
- `import` 不属于 `stat`，因此不能出现在块、函数体或结构体体内；写在这些位置会在 parser 阶段报错。

### 3.2 语句

```ebnf
stat              ::= stat-expr
                    | struct-decl
                    | func-decl
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

for-stat          ::= "for" expr block
                    | "for" expr block "else" block

ret-stat          ::= "ret" NL
                    | "ret" expr NL

break-stat        ::= "break" NL

continue-stat     ::= "continue" NL

stat-expr         ::= final-expr NL
                    | var-def NL
```

说明：

- `break`、`continue` 当前只允许出现在 `for` 循环体里。
- `for ... else ...` 的 `else` 块只在循环条件自然结束时执行；如果循环被 `break` 提前打断，则不会进入 `else`。
- `continue` 只会跳回下一轮条件检查，不会永久屏蔽后续“自然结束后进入 `else`”这一行为。

### 3.3 结构体与函数声明

```ebnf
struct-decl       ::= "struct" IDENT "{"
                      ( struct-stat | NL )
                      { NL | struct-stat }
                      "}"

struct-stat       ::= var-decl NL
                    | func-decl

func-decl         ::= "def" IDENT "(" ")" block
                    | "def" IDENT "(" ")" type-name block
                    | "def" IDENT "(" param-decl-seq ")" block
                    | "def" IDENT "(" param-decl-seq ")" type-name block

var-decl          ::= IDENT type-name
param-decl        ::= IDENT type-name
                    | "ref" IDENT type-name

param-decl-seq    ::= param-decl
                    | param-decl-seq "," param-decl
```

### 3.4 变量定义

```ebnf
var-def           ::= "var" var-decl
                    | "var" var-decl "=" expr
                    | "var" var-decl "=" array-init
                    | "ref" IDENT type-name "=" expr
                    | "ref" IDENT type-name "=" array-init
                    | "var" IDENT "=" expr
                    | "var" IDENT "=" array-init
                    | IDENT ":" "=" expr
                    | IDENT ":" "=" array-init
```

说明：

- 当前 array init 先收口为“初始化语法”，而不是任意位置都可用的裸表达式。
- 变量初始化和后续 `=` / `op=` 赋值当前都按值语义处理；除显式指针外，不会因为赋值自动建立别名。

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
                      "}"
                    | "{"
                      NL
                      brace-line-body
                      "}"

brace-inline-body ::= brace-init-item
                    | brace-inline-body "," NL* brace-init-item

brace-line-body   ::= NL* brace-line-seq NL*

brace-line-seq    ::= brace-init-item
                    | brace-line-seq NL NL* brace-init-item

brace-init-item   ::= expr
                    | brace-init
```

说明：

- `cast-expr` 是内建静态转换表达式；当前 `cast[T](expr)` 只支持内建标量和指针，不支持结构体、tuple、固定维数组等复合类型。
- 当前 `xxx(...)` 统一视为“括号应用”语法；具体是函数调用、函数指针调用，还是未来的数组访问 / 其它重载行为，由后续语义阶段决定。
- `expr-assign-left` 在 parser 层包含 `field-call`，因此 `a(1)`、`grid(1, 2)` 这类数组索引写法可以出现在赋值左侧。
- 但语义阶段只接受“解析成数组索引”的那部分 `field-call` 作为左值；普通函数调用、构造函数调用仍然不是可赋值目标。
- 当前 `aaa.bbb(...)` 除了结构体方法，也可以命中“成员函数注入”入口；位模式视图 `aaa.tobits()` 和 `u8[N].toXXX()` 都走这条路径，但后端会直接 lower 成高效 byte-copy，不生成真实函数调用。
- 普通函数调用和构造函数调用共用同一套参数语法；`Vec2(x=1, y=2)` 与 `mix(x=1, y=2)` 在 parser 层没有分成两套节点。
- 如果形参是 `ref`，调用点也必须显式写 `ref`，例如 `inc(ref x)`、`inc(ref value = x)`；隐式 `ref self` 方法接收者不要求在调用点额外写这个标记。
- 位置实参允许出现在命名实参前面，例如 `mix(1, y=2)`；命名实参后面不能再跟位置实参。
- 元组字面量当前已经支持“显式 tuple 目标类型 + 构造/传递”这一最小闭环。
- 元组成员访问沿用 `field-selector` 规则，字段名按 `_1`、`_2`、`_3` 这种自动生成名称访问。
- 固定维度数组现在已经支持花括号显式初始化、零补齐与 `()` 索引 lowering。
- 花括号初始化当前用于数组。
- 花括号初始化在内部会先形成一个 `initial_list` 风格抽象，再按目标数组类型物化；`initial_list` 当前不是用户可写类型。
- 结构体类型名可以直接作为默认构造函数调用目标，例如 `var c = Complex(real = 1, img = 2)`。
- 普通顶层函数不能与结构体同名，因为 `Type(...)` 语法保留给该类型的构造函数集合。
- 命名实参与位置实参可以混排，但顺序必须与 Python 类似：先位置、后命名。
- 数组初始化按容器层级递归匹配；例如 `i32[4][5]` 适合写成 `{{1}, {2}}`，缺失元素会自动补零。
- 当前语言没有单独的“引用”类型；结构体、tuple、固定维数组等仍默认按值参与赋值与传递，但现在支持 `ref a T = x` 局部别名绑定、`def f(ref x T)` 引用参数，以及隐式 `ref self` 方法接收者。
- 普通 `ref` 形参要求调用点同步写出 `ref` 标记；方法调用里的 `self` 仍保持接收者风格语法，不额外暴露 `ref`。

### 3.6 类型语法

```ebnf
type-name         ::= postfix-type

postfix-type      ::= type-primary
                    | postfix-type "*"
                    | postfix-type "[" "*" "]"
                    | postfix-type "[" "]"
                    | postfix-type "[" expr-seq "]"
                    | postfix-type "[" "," expr-seq "]"
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
- 连续 `[]` 和单个 `[,]` 当前都已进入类型语法，但它们在语义上表示不同的容器组合方式。
- `base-type "[*]"` 现在表示稳定可用的“可索引指针”类型。
- `base-type "[]"` 这种显式未定长数组类型写法对用户是禁止的；如果想省略数组维度，请用 `var a = {1, 2}` 这类初始化器推断。
- 当前编译器会对 `T[]` 直接报 targeted diagnostic；旧写法 `T[]*` 也会给出迁移到 `T[*]` 的明确提示。

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

- 未定长数组语义 (`T[]`)
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

当前实现继续禁止裸函数类型变量；现在这类写法会在 parser 阶段直接报错，因为裸函数签名本身不是用户可见的值类型。
此外，函数指针变量要求在 `var` 定义时立刻给出初始化值，避免未初始化的野函数指针。

例如下面这种写法会报错：

```text
var cb (i32: i32)
```
