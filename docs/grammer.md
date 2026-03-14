# Lona 语言语法文档

> 依据 `grammar/lexer.lex` 与 `build/gen.yacc` 当前实现整理。本文描述的是解析器当前实际支持的语法，不包含临时示例文件中的未来设计。

## 示例索引

- 程序结构与顶层项：[program.md](/home/lurker/workspace/compiler/lona/docs/program.md)
- 变量定义：[vardef.md](/home/lurker/workspace/compiler/lona/docs/vardef.md)
- 函数声明：[func.md](/home/lurker/workspace/compiler/lona/docs/func.md)
- 结构体声明：[struct.md](/home/lurker/workspace/compiler/lona/docs/struct.md)
- 控制流与块语句：[controlflow.md](/home/lurker/workspace/compiler/lona/docs/controlflow.md)
- 表达式：[expr.md](/home/lurker/workspace/compiler/lona/docs/expr.md)
- 类型写法：[type.md](/home/lurker/workspace/compiler/lona/docs/type.md)

## 1. 词法规则

### 1.1 标识符与字面量

- 标识符 `IDENT`：`[a-zA-Z_][a-zA-Z0-9_]*`
- 整数字面量 `INT`：`[0-9]+`
- 浮点字面量 `FLOAT`：`[0-9]+\.[0-9]+`
- 字符串字面量 `STRING`：`"[^"]*"`
- 布尔字面量：`true`、`false`

说明：

- `INT`、`FLOAT`、`STRING` 在语法层统一归为 `CONST`。
- 字符串使用双引号包裹；按当前词法模式 `\"[^\"]*\"`，字符串内部不能再出现任何 `"` 字符，因此 `\"` 这类带引号的转义写法也不在该词法规则的匹配范围内。
- 在词法匹配成功后，扫描器还会对字符串内容调用 `strEscape(...)`；但具体支持哪些转义规则，不由 `lexer.lex` 或 `gen.yacc` 直接定义。

### 1.2 关键字

语句与声明关键字：

- `var`
- `def`
- `ret`
- `if`
- `else`
- `for`
- `struct`

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
- 在 `{` 或 `}` 之后，如果后续匹配到一段形如 `[ ]*\n[ \n]*` 的空白块，这整段都会被词法器吞掉，因此块语句边界附近通常不需要额外处理换行。

### 1.4 词法器可识别的符号

```text
+  -  *  /  !  ~  <  >  |  &  ^
+= -= == != && || { } : = ( ) [ ] @ # , .
```

其中一部分符号虽然在词法器中可识别，但当前 yacc 语法并未使用，详见“当前实现限制”。

## 2. 记号约定

为便于阅读，下面的文法摘要使用以下记号名：

- `IDENT`：普通标识符，对应 `FIELD`
- `BuiltinType`：内建类型关键字，对应 `TYPE`
- `CONST`：整型、浮点、字符串常量
- `BOOL`：`true` 或 `false`
- `NL`：换行

说明：

- `build/gen.yacc` 中顶层开始符号名写作 `pragram`。本文统一记作 `program`，仅做可读性修正。

## 3. 语法摘要（EBNF）

以下是对 `build/gen.yacc` 的等价整理，使用了更易读的 EBNF 风格。

### 3.1 程序结构

```ebnf
program           ::= program-item { program-item }

program-item      ::= NL
                    | program-stat

program-stat      ::= stat
                    | builtin-type-line

builtin-type-line ::= BuiltinType { BuiltinType } NL
```

说明：

- `builtin-type-line` 是一个特殊兼容规则：顶层单独写一行一个或多个内建类型名也能通过解析，但不会形成实际语义结构。
- 当前文法不接受完全空文件；至少需要一个顶层项或一个换行。

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

说明：

- `if` 和 `for` 的条件部分直接是 `expr`，不要求额外的圆括号。
- 表达式语句与 `ret` 语句必须以换行结束。
- 复合语句、`if`、`for`、函数定义、结构体定义通过 `}` 收束，不需要分号。

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

说明：

- 形参与结构体字段都使用 `name type` 形式。
- 函数返回类型如果存在，直接写在参数列表后面，例如 `def add(a i32, b i32) i32 { ... }`。
- 当前结构体定义要求花括号内至少出现一个字段声明或方法声明，不支持空结构体。

### 3.4 变量定义

```ebnf
var-def           ::= "var" var-decl
                    | "var" var-decl "=" expr
                    | "var" IDENT "=" expr
                    | IDENT ":" "=" expr
```

通常最后一种会写成：

```text
name := expr
```

说明：

- `var name type` 表示显式类型声明但不带初始化。
- `var name = expr` 表示省略显式类型、由后续语义阶段推导或处理。

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

expr-assign-left  ::= variable
                    | expr-getpointee

expr-binop        ::= expr "*" expr
                    | expr "/" expr
                    | expr "+" expr
                    | expr "-" expr
                    | expr "<" expr
                    | expr ">" expr
                    | expr "&" expr
                    | expr "==" expr
                    | expr "!=" expr

expr-unary        ::= "!" single-value
                    | "~" single-value
                    | "+" single-value
                    | "-" single-value
                    | "&" single-value
                    | expr-getpointee

expr-getpointee   ::= "*" single-value

expr-paren        ::= "(" expr ")"

typed-value-op    ::= IDENT
                    | CONST
                    | BOOL
                    | expr-paren

single-value      ::= variable
                    | CONST
                    | BOOL
                    | BuiltinType typed-value-op
                    | field-call
                    | expr-paren

field-call        ::= single-value "(" ")"
                    | single-value "(" expr-seq ")"

variable          ::= IDENT
                    | field-selector

field-selector    ::= single-value "." IDENT

expr-seq          ::= expr
                    | expr-seq "," expr
```

说明：

- `BuiltinType typed-value-op` 只接受内建类型关键字开头，例如 `i32 1`、`bool true`。
- 函数调用的被调用者是 `single-value`，因此允许链式形式，如 `obj.method(x)`、`getter()(x)`、`a.b.c()`.
- 成员访问写作 `value.field`。
- 赋值左值既可以是变量，也可以是解引用表达式 `*value`。
- 一元运算的直接操作数是 `single-value`，因此像 `!!x`、`**p` 这样的连续一元写法并不直接出现在当前文法中，通常需要借助括号改写。

### 3.6 类型语法

```ebnf
type-name         ::= base-type
                    | func-head type-name

base-type         ::= single-type
                    | ptr-type
                    | array-type

single-type       ::= IDENT
                    | BuiltinType
                    | type-selector

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

- `type-selector` 支持点分类型名，例如 `pkg.Type`、`a.b.C`.
- 指针类型写作 `T*`。
- 数组类型支持三种形式：`T[]`、`T[n]`、`T[,n]`。最后一种表示首维未指定，后续维度由表达式序列给出。
- 函数类型必须写成 `func-head type-name`，也就是“参数列表头 + 返回类型”。当前语法中不存在只写 `()` 或 `(T1, T2)` 就构成完整类型的规则。
- 由于 `func-head` 自身还可继续接 `*` 或数组后缀，因此这套规则也支持“函数类型的指针/数组”组合写法。

## 4. 运算符优先级与结合性

按 `build/gen.yacc` 中的声明，从低到高如下：

| 优先级 | 运算符 | 结合性 |
| --- | --- | --- |
| 1 | `=` `+=` `-=` | 右结合 |
| 2 | `==` `!=` | 左结合 |
| 3 | `&` `|` | 左结合 |
| 4 | `<` `>` | 左结合 |
| 5 | `+` `-` | 左结合 |
| 6 | `*` `/` | 左结合 |
| 7 | `.` | 左结合 |
| 8 | `()` `[]` | 左结合 |
| 9 | 一元 `!` `~` `+` `-` `&` `*` | 右结合 |

说明：

- 上表是 yacc 中声明的优先级顺序。
- 上表不等于“全部已实现的表达式运算符”。
- `|` 虽然出现在优先级声明里，但当前没有对应的 `expr-binop` 规则。
- `[]` 也出现在优先级声明里，但当前并不是表达式下标运算；`[` `]` 只在类型语法中用于数组类型。

## 5. 当前实现限制

以下内容是根据 `lexer.lex` 与 `gen.yacc` 的组合状态得出的“当前实现现状”：

### 5.1 词法已识别但语法未接入

词法器可以识别下列记号，但 `build/gen.yacc` 目前没有与之对应的正式语法规则，或未形成完整能力：

- `&&`
- `||`
- `|`
- `^`
- `@`
- `#`

其中：

- `|` 出现在优先级声明里，但没有对应的 `expr-binop` 规则。
- `&&`、`||` 虽然有 token 定义，也没有出现在表达式产生式中。
- `@`、`#` 只在词法层返回字符 token，语法层未使用。

### 5.2 词法层存在占位关键字

词法器单独匹配了以下单词，但动作为空，因此当前会被直接忽略，而不会进入语法分析：

- `class`
- `trait`
- `case`
- `cast`

### 5.3 元组语法未完成

`build/gen.yacc` 中存在：

```ebnf
tuple-expr ::= "(" expr "," expr-seq ")"
```

但该产生式没有对应的语义构造动作，`$$` 也未被设置，因此它更像是“预留入口”而不是当前可用的正式语法。若要把它当作正式特性使用，还需要补齐后续语义与 AST 处理。

## 6. 最小有效语法片段

下面这些形式都能从当前文法直接得到：

```text
var x i32
var x i32 = 1
var x = 1
x := 1

def add(a i32, b i32) i32 {
    ret a + b
}

struct Point {
    x i32
    y i32
}

if a < b {
    ret a
} else {
    ret b
}

for cond {
    ret
}
```

以上示例只使用了 `grammar/lexer.lex` 与 `build/gen.yacc` 中已明确实现的语法形态。
