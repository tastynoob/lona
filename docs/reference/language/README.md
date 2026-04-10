# Language Reference

这里收纳当前 `lona` 的语言表面语法与已接通语义。

优先阅读：

1. [grammar.md](grammar.md): 语法总览与文法入口。
2. [expr.md](expr.md): 表达式、调用、运算符和字面量。
3. [type.md](type.md): 类型写法、指针、数组、tuple 和函数指针。
4. [generic.md](generic.md): generic v0、`Type[...]`、`name[T](...)`、推断、single bound 与当前限制。
5. [trait.md](trait.md): `trait`、`impl Trait for Type { ... }`、`value.Trait.method(...)`、`Trait dyn` 和动态 / 静态分派边界。

按主题查阅：

- [mutability.md](mutability.md): `var` / 前缀 `const` / `set` / `ref` 的统一规则。
- [program.md](program.md): 程序结构、顶层项和入口约定。
- [global.md](global.md): 模块级 `global` 与 `#[extern] global`。
- [vardef.md](vardef.md): 变量定义、初始化和 `inline` 编译期常量规则。
- [func.md](func.md): 顶层函数、参数和返回值。
- [generic.md](generic.md): 泛型结构体、泛型函数、实例化、推断与 single bound。
- [struct.md](struct.md): 普通结构体、字段、call-like 初始化和方法语义。
- [trait.md](trait.md): trait 声明、显式 impl body、显式 trait 路径调用和 `Trait dyn`。
- [controlflow.md](controlflow.md): `if`、`for`、`break`、`continue`、`ret`。
- [pointer.md](pointer.md): 显式指针、取地址、解引用和 `T[*]`。
- [ref.md](ref.md): 显式 `ref` 绑定与 `ref` 参数。
