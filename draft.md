# 模块与名字查找重构草案

目前需要重构模块、作用域和名字查找管理，避免继续出现“名字查找一致性问题”。

更准确地说，这次要统一的不是“模块是不是 object”这一件事，而是：

- 模块
- 类型
- 普通值

都应当支持统一的“操作分派协议”。

这样像下面这类语法：

```lona
var a = point.add(1)
```

在前端分析阶段就不需要先假定 `point` 是什么。

它可能是：

- 一个结构体值
- 一个 imported 模块别名
- 一个类型对象
- 一个基础类型值，例如未来支持注入成员的 `i32`

因此前端真正需要做的，不是先判断“`point` 是模块还是结构体”，而是：

1. 先把 `point` 解析成某种语言实体
2. 再统一查询这个实体是否提供成员 `add`
3. 最后根据查找结果决定这是字段、方法、模块成员函数、类型成员，还是非法用法

## 1. 设计目标

这轮重构的目标有 4 个。

- 统一当前模块和 imported 模块的名字查找行为
- 统一 `obj.xxx`、`module.xxx`、`Type.xxx`、注入成员这几类成员访问入口
- 让 `Call` 在 HIR 之前尽量保持 call-like，不要过早写死语义
- 保留现有 `CompilationUnit` / `ModuleInterface` 的构建和增量职责，不把它们误并入运行时对象系统

## 2. 需要区分的三层概念

当前实现里有一个容易混淆的问题：作用域、名字空间、构建单元并不是一回事。

这三层需要明确分开。

### 2.1 词法作用域

词法作用域负责局部绑定查找，例如：

- 局部变量
- 函数参数
- `self`
- `ref` 别名

这一层对应“当前代码块里能直接按名字拿到的局部对象”。

### 2.2 顶层名字空间

顶层名字空间负责模块级名字，例如：

- 顶层函数
- 顶层类型
- imported 模块别名
- 类型名隐含的构造入口

这一层需要支持统一查找接口，不能再分别用：

- 当前模块路径一套逻辑
- imported 模块成员路径一套逻辑

### 2.3 构建单元

构建单元仍然是 `CompilationUnit` / `ModuleInterface` 这一层，负责：

- 源文件身份
- import 图
- 接口采集
- hash / artifact / 增量编译

这一层不应被“模块对象化”直接替换。

也就是说：

- `CompilationUnit` 是构建概念
- “模块成员查找”是语义概念

这两层应当协作，但不能混为一谈。

## 3. 统一抽象：Entity 协议与操作分派

这次重构建议引入一个统一的语义层抽象，可以暂时叫：

- `Entity`
- 或 `EntityRef`

名字可以再定，但重点不在“类名”，而在这组统一操作。

这个抽象的核心是：

- 不要求它一定是运行时值
- 不要求一定新建一个重量对象层
- 只要求它能统一承接“点运算”和“括号应用”

也就是说，这里的 `Entity` 更适合作为一种“鸭子类型”或轻量句柄，而不是一棵新的继承树。

实现上更推荐：

- 一个轻量的 tagged handle
- 或一个不拥有对象的 protocol view
- 或一个小型 tagged union

而不推荐：

- 每解析一个名字就额外 `new` 一个语义对象

否则很容易在现有 AST / resolve / HIR 之外再堆出一层新的对象层级，代价和复杂度都偏高。

### 3.1 Entity 可能代表的实体

`Entity` 至少应覆盖下面几类语义实体：

- 局部值 / 全局值
- 模块名字空间
- 类型对象
- 构造入口集合
- 函数或函数组

例如，不局限于成员访问：

```lona
point(1)
point.add(1)
modA.foo()
Vec2.zero()
1.tof32()
```

在语义分析里都可以理解为：

1. 先解析左边的 `point` / `modA` / `Vec2` / `1`
2. 得到一个轻量的 `Entity`
3. 对该 `Entity` 施加统一操作，例如 `dot(member)` 或 `applyCall(args)`

### 3.2 Entity 应提供的统一操作

这次重构建议明确下面这组接口能力。

#### `kind`

用于描述当前实体是什么。

例如至少应区分：

- module
- type
- object / value
- function
- constructor-set

#### `dot(member)`

用于处理点运算。

```text
entity.dot("add") -> LookupResult
```

这条接口统一承接：

- 模块成员查找
- 结构体字段查找
- 结构体方法查找
- 类型成员查找
- injected member 查找

也就是说，前端不需要先问“这是模块还是结构体”，而是直接问“这个实体能不能响应 `.add`”。

这里还需要补一条约束：

- `dot(member)` 的返回值不能只是一个“终止态描述”
- 它应当能够继续参与后续操作

也就是说，像下面这种链式写法：

```lona
modA.Point.zero().norm()
```

不应在每一步都退回外围重新做一轮手写分支。

更合理的做法是：

- `dot(member)` 返回的 `LookupResult` 内部应携带“后续可继续操作的结果实体”
- 上层可以继续对该结果应用 `dot(...)` 或 `applyCall(...)`

这样成员访问、静态成员、构造结果、方法结果都能落在同一套链式模型里。

#### `applyCall(args)`

用于处理括号应用。

```text
entity.applyCall(callArgs) -> LookupResult / CallResolution
```

这条接口应统一承接：

- 普通函数调用
- 函数指针调用
- 构造调用
- 未来可能的可调用对象

这里的 `callArgs` 不应只是“已经求值后的值列表”。

它至少应保留调用语法本身需要的信息，例如：

- 位置参数 / 命名参数
- `ref` 标记
- 参数名
- 源码位置

否则像下面这些规则就无法在统一接口里正确处理：

- `func(ref x)`
- `func(ref a = x)`
- 构造调用不接受 `ref`

例如：

- 如果 `point` 是函数或函数指针，那么 `point(1)` 是合法调用
- 如果 `point` 是类型，那么 `point(1)` 应走构造解析
- 如果 `point` 是模块，那么 `point(1)` 应明确报错“模块不支持括号应用”

这样 `point(1)` 和 `point.add(1)` 就能落在同一个总模型里，而不是分别走两套先验分支。

#### `asModule` / `asType` / `asObject`

统一操作之外，还应保留少量“安全下转型”接口，用于在必须读取真实载体时取回底层对象。

例如：

- `asModule()`
- `asType()`
- `asObject()`

这些接口的作用不是鼓励上层到处做手写分支，而是：

- 在统一操作已经完成以后
- 某些 lowering 或诊断细节确实需要访问底层具体载体时
- 提供受控的取回路径

因此这组接口应被视为：

- fallback
- escape hatch

而不应成为主路径。

主路径仍应是：

- `dot(member)`
- `applyCall(callArgs)`

否则实现很容易重新退回“到处 `switch(kind)` + `asType/asModule/asObject`”的旧模式。

### 3.3 为什么先统一操作，而不是先统一类别

当前问题的核心并不是“编译器不知道 `point` 是什么类别”，而是：

- 一旦遇到 `.` 或 `()`
- 前端没有统一的操作分派协议
- 于是不得不在很多位置手写“如果是模块...如果是结构体...如果是 injected member...”

因此更合理的建模是：

- 先把名字解析成一个可响应操作的 `Entity`
- 再由 `dot` / `applyCall` 决定后续分派

而不是：

- 先急着把它塞进某个重量对象类别
- 然后在外围不停做 `switch(kind)` 特判

### 3.4 LookupResult 不能只有“找到了”

统一接口不代表“查到成员就行”。

返回值必须显式携带结果类别，否则 HIR lowering 仍然会失去必要信息。

至少应区分：

- 字段
- 方法
- 普通函数值
- 模块成员函数
- 模块成员类型
- 类型成员
- 构造入口集合
- 注入成员
- not found
- ambiguous

除此之外，`LookupResult` 最好还能携带：

- 当前操作解析出的结果实体

也就是“这一步操作之后，后续链式调用应该继续作用在谁身上”。

没有这层信息的话，像下面这种表达式就仍然容易被拆散成多段外部特判：

- `modA.Point.zero().norm()`
- `obj.factory().build().run()`

也就是说，统一的是“查找入口”，不是“所有成员都长一样”。

## 4. 模块在语义层的定位

这里可以继续保留“imported 模块可视为一种对象”的直觉，但要更准确地表述。

更准确的说法应该是：

- imported 模块应当在语义层表现为一种可做成员查找的实体
- 但它不一定是运行时意义上的值对象

因此推荐的实现方向不是“万物都继承运行时 `Object`”，而是：

- 模块、类型、值都实现统一的操作分派协议

这比单纯说“模块也是 object”更稳。

原因是模块和类型并不天然具备下面这些运行时语义：

- `get()`
- `set()`
- 取地址
- 参与 IR value 运算

但它们确实应当具备：

- 名字查找
- 成员查找
- 作为 call-like callee 被进一步分析

## 5. import 的统一语义

这次重构建议明确下面这条语义：

```lona
import modA
```

逻辑上等价于：

- 在当前模块的顶层名字空间中引入一个名为 `modA` 的模块实体

这样：

```lona
modA.xxx
```

的处理流程就可以统一成：

1. 先按普通名字查找拿到 `modA`
2. 确认它是一个支持成员查找的模块实体
3. 再对它做统一的 `dot("xxx")`

关键点在于：

- imported 模块成员访问不应再拥有单独的一套搜索顺序
- 它只是“先找到一个模块实体，再对其查成员”

这与普通的：

```lona
point.add
```

在语义层应当走同一大框架。

## 6. 类型名与构造函数入口

当前语言已经明确：

- `Type(...)` 表示构造调用
- `struct Name` 会占用 `Name(...)` 这一组构造入口
- 普通顶层函数不能与类型名同名

这条规则应在本草案中继续保持，不应回退。

因此顶层名字查找时，类型名不只是一个“type”。

它还隐含：

- 默认构造函数入口
- 未来可能扩展出的构造函数重载集合

所以对于类型相关查找，统一接口返回的结果应能表达：

- 类型对象
- 构造入口集合

不能再退化成简单的 `findType()` 布尔判断。

## 7. 建议的前端处理流程

### 7.1 Parser / AST

Parser 不需要过早区分：

- 普通函数调用
- 构造调用
- 模块成员函数调用

只需要保留统一语法：

- `Field`
- `Selector`
- `Call`

### 7.2 Resolve

Resolve 阶段负责把名字解析为统一的语义实体，而不是急着决定最终行为。

建议：

- 普通名字解析为 `EntityRef`
- selector base 也解析为 `EntityRef`
- selector 本身通过统一 `dot(member)` 产生 `LookupResult`
- call callee 通过统一 `applyCall(args)` 产生 `CallResolution`

这一步的核心职责是：

- 决定“这是什么实体”
- 决定“这个实体是否支持当前操作”

而不是马上决定最终 lowering 细节。

### 7.3 HIR Lowering

到 HIR lowering 时，再根据 `LookupResult` / `CallResolution` 分流为：

- 普通函数调用
- 函数指针调用
- 构造调用
- 方法调用
- 字段访问
- 数组索引
- injected member 调用

也就是说：

- 前面统一入口
- 后面按结果类型分流

这样既统一了搜索逻辑，又保留了清晰的语义边界。

## 8. 与当前代码的关系

当前仓库里其实已经有一些可复用的雏形：

- `Object`
- `ModuleObject`
- `TypeObject`
- `CompilationUnit`
- `ModuleInterface`

但它们目前分散在不同层次，各自承担了一部分“名字查找”职责。

现在真正的问题不是“完全没有这些概念”，而是：

- 词法作用域查找用一套逻辑
- 当前模块顶层 type/function 查找用一套逻辑
- imported 模块成员查找又用一套逻辑
- selector、call、constructor 的分流也分散在多处

因此重构重点应该是“统一接口并重新分层”，而不是推倒全部已有结构。

## 9. 建议的数据结构方向

这里只给方向，不强行锁死类名。

### 9.1 顶层名字空间

建议增加一层统一抽象，例如：

```text
TopLevelNamespace
```

它提供统一接口：

```text
lookupTopLevelName(name) -> LookupResult
```

当前模块和 imported 模块都应各自暴露一个 `TopLevelNamespace`。

### 9.2 语义实体

建议增加统一抽象，例如：

```text
EntityRef
```

它代表 resolve 之后的“某种可参与点运算和调用分析的实体”。

这里建议把 `EntityRef` 设计成轻量句柄，而不是重量对象。

更具体地说，它更像：

- `kind + pointer/reference`
- 一组非拥有式视图
- 一层小的协议适配器

而不是每次查找后额外分配一层新的语义节点。

例如：

- LocalValueEntity
- GlobalFunctionEntity
- ModuleEntity
- TypeEntity

其中至少应具备下面这些统一成员：

- `kind`
- `dot(member)`
- `applyCall(args)`
- `asModule()`
- `asType()`
- `asObject()`

### 9.3 成员查找结果

建议增加统一结果类型，例如：

```text
LookupResult
```

其中至少包含：

- kind
- 对应的目标实体或类型信息
- 可继续参与后续 `dot/applyCall` 的结果实体
- 诊断辅助信息

## 10. 建议的接口草图

这里只是说明方向。

```text
lookupName(lexicalScope, name) -> EntityRef
entity.dot(name) -> LookupResult
entity.applyCall(callArgs) -> CallResolution
```

对于 imported 模块来说：

```text
lookupName(currentScope, "modA") -> ModuleEntity
ModuleEntity(modA).dot("Counter") -> TypeEntity / ConstructorSet
ModuleEntity(modA).dot("foo") -> FunctionEntity
ModuleEntity(modA).applyCall(callArgs) -> error: module is not callable
```

对于普通值来说：

```text
lookupName(currentScope, "point") -> ValueEntity
ValueEntity(point).dot("x") -> Field
ValueEntity(point).dot("add") -> Method
ValueEntity(point).applyCall(callArgs) -> depends on whether point is callable
```

对于 injected member 来说：

```text
ValueEntity(i32).dot("tof32") -> InjectedMember
```

## 11. 局部作用域与模块别名遮蔽

这里建议先明确一个工程上更稳的约束：

- imported 模块别名第一版不允许被局部变量同名遮蔽

原因是如果允许：

```lona
import math

def main() {
    var math = 1
    math.abs()
}
```

那么 `math.xxx` 的意义会在局部突然变化，查找模型会立刻变得更复杂。

这条规则不是永远不能放开，但第一版最好先禁止。

## 12. 建议的重构顺序

建议按下面顺序推进。

1. 保持现有“类型名独占构造入口”的语言约束不回退。
2. 抽出统一的顶层名字空间抽象，不改 lowering 语义。
3. 让当前模块和 imported 模块都走同一套顶层查找接口。
4. 把 selector 的模块成员查找、结构体成员查找、注入成员查找收拢到统一的 `dot(member)` 协议。
5. 把 callee 的函数调用、构造调用、函数指针调用收拢到统一的 `applyCall(args)` 协议。
6. 让 resolve 输出统一的 `EntityRef`，并在其上驱动 `LookupResult / CallResolution`。
7. 在 HIR lowering 中根据结果 kind 再做分流。
8. 最后再考虑显式构造函数重载和更复杂的静态成员模型。

## 13. 这版草案的核心结论

这次重构要统一的核心，不是“模块到底算不算 object”。

真正需要统一的是：

- 名字查找接口
- 点运算接口
- 括号应用接口
- call-like 语法的前端建模

更准确的目标表述应当是：

- 模块、类型、值都应当实现统一的操作分派协议
- `CompilationUnit` 继续保留为构建单元
- imported 模块成员查找与当前模块名字查找必须复用同一套顶层名字解析逻辑

这样才能从根上消除当前“本地路径一套、imported 路径一套”的工程问题。
