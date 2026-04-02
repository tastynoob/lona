# Generic v0 Internals

本文记录当前仓库里已经实现的 generic v0 内部模型，目标是回答三类问题：

- generic 相关语法节点在前端是怎样建模的
- 泛型模板信息怎样进入 `ModuleInterface`
- 为什么当前已经能做接口收集、类型校验和调用点推断，但还没有进入真正的 monomorphization / runtime symbol 生成

这不是设计草案；这里描述的是当前代码已经在做的事。  
用户侧语法和语言层规则请看 `docs/proposals/generic_v0.md`；这里专注“编译器内部现在怎样表示与处理 generic v0”。

## 1. 当前范围

当前仓库里已经落地的 generic v0 主要覆盖：

- `struct Box[T]` / `def id[T](...)` / `impl[T] Box![T]: Trait` 的语法与 AST
- `Name![T]` / `func![T](...)` / `func![T]&<>` 的 surface 解析
- `AppliedTypeNode` / `AnyTypeNode` 的类型节点与接口哈希
- generic template 的接口收集、owner-context 类型解析与 body 校验
- generic call 的类型实参数量检查、保守推断和签名替换

当前明确**还没有**落地：

- 真正的 generic function / generic struct 实例化
- concrete applied struct layout 物化
- importer-owned instance artifact
- bound 语法与 bound satisfaction
- monomorphized runtime symbol emission

所以当前 generic v0 的内部状态更准确地说是：

- “前端 / 接口 / 校验通路已接通”
- “runtime instantiation 仍未实现”

这也是为什么当前许多正例会稳定走到：

- `generic function instantiation is not implemented yet`

而不是再掉回 parser / name-resolution 的旧错误。

## 2. AST 与类型节点

generic v0 相关的 AST / type-node 入口主要在：

- `src/lona/ast/astnode.hh`
- `src/lona/ast/astnode_toJson.cc`
- `src/lona/ast/type_node_tools.cc`
- `src/lona/ast/type_node_string.cc`
- `grammar/main.yacc`
- `grammar/type.sub.yacc`

当前前端新增了四类关键表示：

- `AstStructDecl::typeParams`
- `AstFuncDecl::typeParams`
- `AstTraitImplDecl::typeParams`
- `AstTypeApply`

其中：

- 声明处仍然使用 `[T]`
- 使用处使用 `Name![T]`
- 表达式侧的 generic call / function ref 也先落成 `AstTypeApply`

类型层新增了两个专用 type node：

- `AnyTypeNode`
- `AppliedTypeNode`

`AppliedTypeNode` 对应：

- `Box![i32]`
- `Pair![T, bool]`
- `dep.Box![i32]`

`AnyTypeNode` 对应：

- `any*`
- `any const*`
- `any[*]`
- `any const[*]`

这里有两个当前实现边界值得明确：

1. `![...]` 只表示 generic apply，不和数组后缀共用 `[]`
2. bare `any` 不是值类型；只有 pointer / indexable-pointer 位置允许使用

旧的方括号 generic apply：

- `Box[i32]`
- `id[i32](1)`

当前在 expression / type 两侧都会被定向诊断为：

- generic apply uses `![...]`, not `[...]`

而不是再误落到数组维度分支。

## 3. 接口层数据模型

generic 信息首先进入 `ModuleInterface`，对应文件：

- `src/lona/module/module_interface.hh`
- `src/lona/module/module_interface.cc`
- `src/lona/declare/interface.cc`

当前接口层新增或扩展了四类 generic 相关数据：

- `ModuleInterface::GenericParamDecl`
- `TypeDecl::typeParams`
- `FunctionDecl::typeParams`
- `TraitImplDecl::typeParams`

另外，generic struct method 当前不会直接变成 runtime method table，而是先保存在：

- `TypeDecl::methodTemplates`

它记录的是：

- method 名
- receiver access
- parameter binding / names
- parameter type node / spelling
- return type node / spelling
- method 自身 type params

这和 monomorphic struct method 的路径不同：

- monomorphic method 会直接进 `StructType` 的 method table
- generic method template 只保留接口元数据，不直接产生 runtime function

当前接口收集阶段做几件事：

1. 先收 generic struct / function / trait impl header
2. 把 type params 记录到 `ModuleInterface`
3. 对 generic 签名做 arity / bare-template / bare-`any` / applied-type 合法性检查
4. 对 generic struct method 保留 template metadata，而不是像早期实现那样“校验后丢弃”

## 4. `interfaceHash` 与可见接口边界

generic v0 没有把这些信息放进“纯实现细节”层，而是明确进入接口哈希，相关代码在：

- `src/lona/module/compilation_unit.cc`

当前 `computeInterfaceHash(...)` 会把下列 generic 相关信息算进 `interfaceHash`：

- struct / function / trait impl 的 `typeParams`
- `AppliedTypeNode`
- `AnyTypeNode`
- direct import 列表

这里 direct import 也要算进哈希，是因为当前 imported generic signature 的语义已经依赖 owner module 的 visible import graph。

典型例子是：

- `dep.take_helper_ptr(...)`

其 exported signature 里如果写了：

- `helper.Box![T]*`

那么 importer 在做推断时，必须以 `dep` 的 owner interface 作为名字解析上下文，而不能按 caller 自己的 import graph 去猜。

因此 generic v0 现在的可见接口边界不只是：

- exported type / function / trait 名字

还包括：

- generic parameter 列表
- applied type 形状
- owner interface 可见的 import alias

## 5. 类型解析：模板名、applied type 与 `any`

generic v0 当前的语义类型解析主要落在：

- `src/lona/module/compilation_unit.cc`
- `src/lona/declare/interface.cc`
- `src/lona/type/type.hh`

### 5.1 bare generic template 不进入 runtime type 语义

`Box` 如果是 generic template，那么这些写法都会被拒绝：

- `var box Box`
- `var p Box*`

当前错误会明确提示：

- generic type template `Box` requires explicit `![...]` type arguments

这条规则的目的很明确：

- generic template 不是普通 runtime type
- 只有 concrete applied type 才能继续进入 runtime 语义

### 5.2 applied type 当前先落成 opaque struct identity

当 `CompilationUnit::resolveTypeNode(...)` 遇到：

- `Box![i32]`
- `Pair![i32, bool]`

它当前不会去物化 concrete field layout，而是先创建一个带 applied-template 元数据的 opaque `StructType`：

- `createOpaqueStructType(...)`

这个类型会保存：

- applied 后的完整显示名
- template exported name
- concrete type argument 列表

因此当前 `Box![i32]*` 可以稳定通过：

- pointer type formation
- signature substitution
- imported signature inference

但 `Box![i32]` 按值使用仍然会被拒绝，因为 concrete layout 还没有实例化出来。

### 5.3 `any` 的内部角色

`any` 在当前实现里不是“万能值类型”，而是受限的 erased pointee type。

内部表示上：

- `AnyTypeNode` 在 AST / type-node 层独立存在
- `ModuleInterface` 和 `CompilationUnit` 都有专门的 `AnyType`

当前允许的只是：

- `any*`
- `any const*`
- `any[*]`
- `any const[*]`

因此 generic v0 当前把 `any` 收得很窄：

- 它服务于显式擦除边界
- 不参与普通 by-value generic storage

## 6. owner-context 解析与 imported generic 签名

generic v0 当前一个比较关键的实现点是：

- imported generic signature 不能再只靠 caller 的本地 lookup 去解析名字

相关代码在：

- `src/lona/resolve/resolve.hh`
- `src/lona/resolve/resolve.cc`
- `src/lona/analyze/function.cc`
- `src/lona/module/module_interface.hh`

当前内部做法是：

1. imported function / type reference 会携带显式 `ownerInterface`
2. generic signature 里的 type name 会优先按 owner interface 的上下文解析
3. 这条 owner-first 规则同时覆盖：
   - unqualified 名字
   - secondary-module-qualified 名字

这样可以避免两类旧错误：

- importer 本地存在同名 `Box`，把 `dep.Box![T]` 的签名推断截走
- owner signature 里写 `helper.Box![T]`，却错误依赖 caller 也 `import helper`

当前 `ModuleInterface` 里记录 direct imported modules，也是为了让这种 owner-context 解析在 importer 侧仍然可重建。

## 7. generic template 的 resolve 与 body 校验

generic template 当前不会像早期那样被整个跳过；它们会进入一个专门的“模板校验但不 lowering”路径，相关代码在：

- `src/lona/resolve/resolve.hh`
- `src/lona/resolve/resolve.cc`
- `src/lona/analyze/module.cc`

核心机制是：

- `ResolvedFunction` 带有 `templateValidationOnly`
- 同时记录 `genericTypeParams`

这条路径的行为是：

1. generic top-level `def`
2. generic struct method

都会进入 name resolution

这样 generic body 里的这些错误现在会在编译模块时被直接拒绝：

- 未定义标识符
- 本地变量使用不可见类型参数
- `cast[...]` / `sizeof[...]` 的非法类型

但它们**不会**继续进入：

- HIR lowering
- runtime symbol emission
- LLVM codegen

`ModuleAnalyzer` 会显式跳过 `templateValidationOnly` 的 resolved function。  
因此当前 generic template 的状态是：

- body 已校验
- runtime 实体未生成

## 8. generic call 的语义阶段

generic function call 当前主要在：

- `src/lona/analyze/function.cc`

当前已经落地的 generic call 语义包括：

- 显式 `func![T](...)`
- 省略 type args 时的保守参数驱动推断
- `func![T]&<>` 这种“先特化、再取具体函数地址”的前端路径

这里有三步关键处理。

### 8.1 先收 type args

`resolveGenericCallTypeArgs(...)` 会：

- 做 `![...]` 个数检查
- 若无显式 type args，则按参数模式推断

当前推断不是字符串替换，而是递归走 type pattern：

- base type parameter
- `const`
- pointer
- indexable pointer
- array
- tuple
- function pointer
- `AppliedTypeNode`

因此像下面这些签名现在都能进入统一的 generic call 路径：

- `T*`
- `Pair![T, bool]*`
- `<Pair![T, bool]*, i32>`
- `Box![T] const*`

### 8.2 再做签名替换

拿到 `T -> i32` 这类映射后，`substituteGenericSignatureType(...)` 会递归把模板签名转成 concrete signature type。

这一步当前同样支持：

- `AppliedTypeNode`
- `AnyTypeNode`
- `DynTypeNode`
- 指针 / 元组 / 函数指针等复合类型

因此 generic call 在语义阶段已经能得到：

- concrete parameter type list
- concrete return type

并完成普通的参数绑定检查。

### 8.3 当前终点仍然是 placeholder

虽然前两步已经接通，但 `analyzeGenericFunctionCall(...)` 当前最后仍然会停在：

- `diagnoseGenericInstantiationPending(...)`

也就是说，当前 generic call 的真实终点仍然不是实例化，而是一个有意保留的占位诊断。

这也是 generic v0 当前最大的内部边界：

- semantic front door 已经完整
- instantiation back end 还没落地

## 9. trait interop 的当前状态

generic v0 和 trait v0 当前已经有一层接口级互操作，但还没有进入 bound / monomorphization 阶段。

已落地的部分在：

- `src/lona/declare/interface.cc`
- `src/lona/module/module_interface.hh`

当前已经支持：

- `impl Box![i32]: Hash`
- `impl[T] Box![T]: Hash`

接口层会保存：

- fully-qualified self type spelling
- trait name
- impl 的 type params

这里还有一个当前实现边界：

- 如果 self type 已经是 concrete monomorphic struct，接口收集会继续做 method-table 对齐校验
- 如果 self type 是 applied / generic self type，当前只保留 impl header，不会去做 concrete method table 验证

换句话说，generic trait impl header 现在已经能稳定进入接口模型；但真正和实例化绑定在一起的 trait satisfaction / bound call 还没有实现。

## 10. 当前没有实现的部分

虽然 generic v0 当前已经把 parser、接口、body 校验和 owner-context 解析打通了，但它仍然没有进入真正的 runtime generic。

当前明确还没有的东西包括：

- generic bound 语法与 `boundTraitName` 的实际消费
- concrete applied struct field layout 物化
- instantiated function / method body 的 substituted HIR
- importer-owned instance record
- instance key / reuse / invalidation
- monomorphized runtime symbol emission

因此当前这套内部实现更适合理解成：

- generic v0 的“前端可验证模板模型”

而不是：

- generic v0 的完整 monomorphization 后端

后续如果真正补齐 runtime 实例化，这份文档最可能需要扩写的部分是：

- applied struct layout materialization
- instantiated function symbol naming
- importer-owned instance cache
- 与 trait impl graph 相关的 invalidation 边界
