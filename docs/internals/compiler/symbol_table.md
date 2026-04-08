# 泛型与 Trait 的符号表模型

本文档约束当前 `lona` 在存在 generic 和 trait 时，内部“符号表”到底分成哪些层、每层存什么、哪些名字会进入哪一层，以及哪些东西明确不应该混在一起。

这里说的“符号表”不是单个 `map<string, ...>`。
当前实现是多层模型：

- 模块接口层的顶层声明表
- `CompilationUnit` 的可见名字叠加层
- resolve 阶段的词法局部作用域栈
- `TypeTable` / `StructType` 里的类型与方法表
- generic runtime 的 instance registry 与 artifact record

如果只盯某一层，很容易误以为“同一个名字已经注册过了”，但实际上它可能只是进入了另一层表。

## 1. 先给结论

当前需要长期保持的约束是：

- 顶层 `struct` / `trait` / `def` / `global` 先收集声明，再完成类型和函数体。
- 因此顶层类型和顶层函数允许“先使用后定义”。
- 局部变量、参数、`self` 绑定不允许这样做；它们走块级、从上到下的词法作用域。
- `trait impl` 不是顶层名字空间成员；它不会进入 `lookupTopLevelName(...)`。
- generic template declaration 会进入顶层名字空间；concrete generic instance 不会。
- trait 方法在类型层不按裸方法名唯一存储，而是按
  `traitMethodSlotKey(traitExportedName, methodLocalName)` 存储。
- generic instance 的去重身份不能靠 mangled symbol string；必须靠结构化 key。
- direct import 才会把模块 alias 放进当前模块顶层名字空间；transitive import 不会。

## 2. 符号表不是一层，而是五层

| 层 | 主要结构 | key | 负责什么 |
| --- | --- | --- | --- |
| 模块接口层 | `ModuleInterface` | 顶层本地名、导入 alias、本地 trait impl 列表 | 保存一个模块“对外可见的声明接口” |
| 模块可见层 | `CompilationUnit` | 本地绑定名、direct import alias | 统一本模块和 imported 模块的顶层查找 |
| 函数 resolve 层 | `FunctionResolver::localScopes_` | 局部变量名 | 块级、从上到下的局部绑定 |
| 类型/方法层 | `TypeTable` + `StructType` | 类型全名、`(StructType*, methodKey)` | canonical 类型、inherent method、trait method 槽位 |
| generic runtime 层 | `GenericInstanceRegistry` + `GenericInstanceArtifactRecord` | structured instance key | concrete generic instance 的去重、发射归属和缓存失效 |

后面每层分别展开。

## 3. 模块接口层：`ModuleInterface`

相关代码：

- `src/lona/module/module_interface.hh`
- `src/lona/module/module_interface.cc`
- `src/lona/declare/interface.cc`

这一层保存的是“模块接口”，不是 lowering 时直接可执行的对象。

### 3.1 顶层声明表长什么样

当前 `ModuleInterface` 里主要有这些表：

- `localTypes_ : localName -> TypeDecl`
- `localTraits_ : localName -> TraitDecl`
- `localFunctions_ : localName -> FunctionDecl`
- `localGlobals_ : localName -> GlobalDecl`
- `importedModules_ : localAlias -> ImportedModuleDecl`
- `traitImpls_ : vector<TraitImplDecl>`
- `derivedTypes_ : spelling -> TypeClass*`

这里最重要的约束有两个：

1. 顶层可见名字和 `trait impl` 分开存
2. template metadata 和 runtime concrete instance 分开存

也就是说：

- `struct Box[T]` 会进入 `localTypes_`
- `def wrap[T](...)` 会进入 `localFunctions_`
- `impl[T Hash] Hash for Box[T] { ... }` 只会进入 `traitImpls_`
- `Box[i32]`、`wrap[i32]`、`Box[i32].get` 这些 concrete instance 不会进入这一层

### 3.2 exported name 与 source local name 分开

这一层已经把“源码里怎么写”和“模块导出时怎么命名”分开了。

例如模块路径是 `string/result` 时：

- `localName = Result`
- `exportedName = string.result.Result`

对应代码在：

- `ModuleInterface::exportedNameFor(...)`
- `ModuleInterface::functionSymbolNameFor(...)`
- `ModuleInterface::globalSymbolNameFor(...)`

因此这一层里至少同时存在两套名字：

- source-facing local name
- export-facing canonical name

这也是为什么 imported 查找不能只比 basename。

### 3.3 trait 在接口层的形状

trait 相关数据分成三部分：

- `TraitDecl`
- `TraitMethodDecl`
- `TraitImplDecl`

其中：

- trait declaration 进入顶层名字空间
- trait method 只作为 trait 自身的签名列表存在
- trait impl 不进入顶层名字空间，只进入 `traitImpls_`

这条边界非常重要，因为 `impl` 不是源码里可直接引用的名字。
也就是说，下面两种东西绝对不能混：

- `Hash` 这样的顶层 trait 名字
- `impl Hash for Point { ... }` 这样的满足性声明

### 3.4 generic 在接口层的形状

generic metadata 当前存放在 declaration 自身，而不是旁路字符串表里：

- `TypeDecl::typeParams`
- `FunctionDecl::typeParams`
- `TraitImplDecl::typeParams`
- `TypeDecl::methodTemplates`

因此接口层表达的是：

- “这个声明是 template”
- “它有哪些 type parameter / bound”
- “如果它有 body method template，签名长什么样”

接口层不表达：

- 这个 template 当前已经实例化了哪些 concrete instance
- 某个 concrete instance 最终在哪个模块发射

那些信息属于第 7 节的 generic runtime 表。

## 4. 模块可见层：`CompilationUnit`

相关代码：

- `src/lona/module/compilation_unit.hh`
- `src/lona/module/compilation_unit.cc`

这一层不是新的接口缓存，而是“当前模块在本轮编译里能看到什么名字”的叠加结果。

### 4.1 当前保存的表

`CompilationUnit` 里当前有：

- `importedModules_ : alias -> ImportedModule`
- `localTypeBindings_ : localName -> resolvedName`
- `localTraitBindings_ : localName -> resolvedName`
- `localFunctionBindings_ : localName -> resolvedName`
- `localGlobalBindings_ : localName -> resolvedName`
- `resolvedTypes_ : TypeNode* -> TypeClass*`
- `recordedGenericInstances_ : vector<GenericInstanceArtifactRecord>`

这里的 `resolvedName` 已经是语义层 canonical 名字，比如：

- 顶层 type / trait 用 exported name
- 顶层 function / global 用最终 symbol name

### 4.2 `lookupTopLevelName(...)` 的边界

当前统一顶层查找入口是：

- `CompilationUnit::lookupTopLevelName(name)`
- `CompilationUnit::lookupTopLevelName(importedModule, name)`

这一层负责把“当前模块自己的名字”和“某个 direct import 模块里的成员名字”统一成同一套 `TopLevelLookup` 结果。

它当前能返回：

- `Module`
- `Type`
- `Trait`
- `Function`
- `Global`
- `NotFound`

它明确不会返回：

- trait impl
- concrete generic instance
- 局部变量

### 4.3 direct import 与 transitive import 的约束

模块 alias 只来自 direct import。

也就是说：

- `import wrap`
- `wrap` 再 `import leaf`

那么在当前模块里：

- `wrap.xxx` 合法
- `leaf.xxx` 不会因为 transitive import 自动进入顶层名字空间

这条约束现在已经收住，不能再回退成“把整个依赖图的 module namespace 都注进 entry scope”。

### 4.4 顶层前向引用为什么成立

顶层类型和函数支持“先使用后定义”，不是因为 resolve 会向后扫描源码，而是因为：

1. declaration collection 先收集顶层声明
2. `CompilationUnit` 先填好本地顶层 binding
3. resolve / analyze 阶段再消费这些 binding

因此下面这种写法是合法的：

```lona
def main() i32 {
    ret helper()
}

def helper() i32 {
    ret 1
}
```

同理，顶层 `struct Outer { inner Inner }` / 后面再定义 `Inner` 也可以成立。

但这条规则只适用于顶层声明，不适用于局部绑定。

## 5. 函数 resolve 层：块级词法作用域

相关代码：

- `src/lona/resolve/resolve.cc`

函数级名字查找不是挂在 `CompilationUnit` 顶层表上的，而是单独一套局部作用域栈：

- `FunctionResolver::localScopes_`

每一层是：

- `unordered_map<string, const ResolvedLocalBinding *>`

当前规则是：

- 进入 block 时 `pushLocalScope()`
- 离开 block 时 `popLocalScope()`
- 查找时从栈顶往外层查

因此局部变量现在是：

- 块级作用域
- 同一层内禁止重复定义
- 只能引用已经声明过的上文绑定

这和顶层声明的规则故意不同：

- 顶层：先收集，再使用
- 局部：从上到下，按块入栈

这条分层必须保持，不要再把“顶层前向引用成立”误推广到局部变量。

## 6. 类型层：`TypeTable` 与 `StructType`

相关代码：

- `src/lona/type/type.hh`
- `src/lona/type/scope.hh`
- `src/lona/declare/interface.cc`
- `src/lona/analyze/function.cc`

这里保存的是 canonical 类型和方法绑定，不是源码顶层名字。

### 6.1 `TypeTable` 里有什么

`TypeTable` 当前主要有两张表：

- `typeMap : full_name -> TypeClass`
- `methodFunctions_ : (StructType*, name) -> Function*`

这里的 key 都不是 source local name：

- 类型用 `full_name`
- 方法用“接收者类型 + method key”

所以不能把这层误认为“又一张顶层名字表”。

### 6.2 `StructType` 里有什么

`StructType` 里当前分开保存三类成员：

- `members`
- `methodTypes`
- `traitMethodTypes`

其中：

- `methodTypes` 的 key 是 inherent method local name，例如 `hash`
- `traitMethodTypes` 的 key 是 trait slot key，例如
  `string.result.Hash::hash`

trait method slot key 的构造函数是：

```text
traitMethodSlotKey(traitName, methodName) = traitName + "::" + methodName
```

这里的 `traitName` 必须是 trait 的 resolved/exported name，而不是源码里某次局部 alias。

### 6.3 为什么 trait method 不能只按裸方法名存

因为同一个 concrete type 可以同时实现多个 trait，它们可能都有相同的 local method name：

- `Hash.hash`
- `Metric.hash`

如果 trait method 只按 `hash` 存，后注册的一份会覆盖前一份。
当前正确做法是：

- 存储时按 `TraitExportedName::methodLocalName`
- dot lookup 时再额外做一次“按 local name 聚合”

这也是为什么 `StructType::findTraitMethodsByLocalName(...)` 可能返回多项。

### 6.4 plain dot lookup 的约束

当前 `value.method()` 的逻辑约束是：

1. 先查字段
2. 再查 inherent method
3. 再查 trait method 的 local-name 聚合结果

如果第 3 步拿到多项，语义必须报二义性，不能偷偷选第一项。

generic trait impl method 的 lazy materialization 也必须遵守这条规则：

- 为了做 plain dot lookup，必须先把所有匹配的可见 trait method 都 materialize 出来
- 不能“找到一个就返回”

否则会把本应二义的 `box.hash()` 错绑成某一个 trait method。

### 6.5 method function binding 的约束

真正的 runtime `Function*` 绑定在 `TypeTable::methodFunctions_` 里，key 是：

- `(StructType*, inherentMethodName)`
- 或 `(StructType*, traitMethodSlotKey)`

也就是说：

- `Point.hash`
- `Point.(Hash::hash)`

在这一层是两条不同记录。

codegen 取方法符号时也依赖这个约束；不能只靠最终 LLVM symbol string 倒推语义。

## 7. generic runtime 层：instance key、emitter 与 artifact record

相关代码：

- `src/lona/module/generic_instance.hh`
- `src/lona/analyze/function.cc`
- `src/lona/module/compilation_unit.cc`
- `src/lona/workspace/workspace_builder.cc`

generic template declaration 和 concrete runtime instance 是两套不同的表。

### 7.1 `GenericInstanceKey` 负责“这是不是同一个实例”

当前 key 结构是：

- `requesterModuleKey`
- `ownerModuleKey`
- `kind`
- `templateName`
- `methodName`
- `concreteTypeArgs`

但 `operator==` 和 hash 故意不把 `requesterModuleKey` 算进 identity。

当前真正的实例身份是：

- `ownerModuleKey`
- `kind`
- `templateName`
- `methodName`
- `concreteTypeArgs`

`requesterModuleKey` 只保留在 artifact record 里，用来描述：

- 这是哪个 requester 模块记录下来的实例使用信息
- 缓存复用时应与哪个 requester artifact 对齐

它不能再决定“这是不是另一个新实例”。

### 7.2 `GenericInstanceRegistry` 负责“由谁发射”

`GenericInstanceRegistry` 当前是：

- `GenericInstanceKey -> GenericInstanceEmissionOwner`

它记录的是某个 concrete instance 最终由哪个模块发射，以及已经发射了哪些 symbol。

因此 generic runtime 的当前模型是：

- same graph 内同一个 concrete instance 只有一个 emitter
- 其它 requester 只记录自己依赖了这个实例
- 不能再让每个 requester 都各自产生一份强定义

### 7.3 `recordedGenericInstances_` 负责“当前 requester 记录了什么”

`CompilationUnit::recordedGenericInstances_` 保存的是本 requester 模块的 artifact 级记录。

这层会记住：

- structured instance key
- template revision
- 当前 artifact 是否负责发射 symbol

这里的 revision 当前包含：

- owner `interfaceHash`
- owner `implementationHash`
- owner `visibleImportInterfaceHash`
- requester `visibleTraitImplHash`

这意味着 generic cache invalidation 不只跟模板源码有关，也跟 bound / visible impl 状态有关。

### 7.4 哪些东西会形成 generic runtime record

当前会形成 record 的主要有：

- generic function instance
- applied generic struct instance
- generic struct method instance
- generic trait impl body method instance

但不是每种 record 都一定有 emitted symbol：

- applied generic struct instance 主要记录 concrete type materialization
- function / method / trait impl method instance 才可能带 emitted symbol

这条边界也要保持，因为“类型 materialization”与“函数体发射”不是一回事。

## 8. 导出名、链接名、内部符号是三套东西

相关代码：

- `src/lona/module/module_interface.cc`
- `src/lona/declare/function.cc`
- `src/lona/declare/interface.cc`
- `src/lona/sema/moduleentry.hh`
- `src/lona/analyze/function.cc`
- `src/lona/emit/codegen.cc`

当前至少有三种“名字”并存：

1. 源码 local name
2. 语义 canonical name / slot key
3. LLVM symbol name

这三者不能混用。

### 8.1 先区分三类名字

#### A. 语义导出名

这类名字进入：

- `ModuleInterface`
- `CompilationUnit` 的 resolved top-level binding
- trait slot key

例如：

- `string.result.make`
- `string.result.Result`
- `string.result.Hash`

它们主要用于“这是什么声明”的语义身份，不一定直接对应 object-file symbol。

#### B. 真实链接符号名

这类名字进入 LLVM module，最终出现在 bitcode / object / linker 里。

例如：

- `string.result.make`
- `string.result.Point.hash`
- `string.result.Point.__trait__.string_2eresult_2eHash.hash`
- `__lona_trait_witness__...`

#### C. 仅内部使用的 helper symbol

这类名字也会进入 LLVM module，但不属于语言用户可引用的顶层导出接口。

例如：

- `__<moduleKey>_init_entry__`
- `__<moduleKey>_init_state__`
- `__<moduleKey>_init_result__`
- `.lona.global.bytes.<id>`

### 8.2 `mangleModuleEntryComponent(...)` 的规则

多处运行时符号会依赖 `mangleModuleEntryComponent(...)`。

当前规则是：

- `a-z` / `A-Z` / `0-9` 原样保留
- 其它每个字节编码成 `_hh`
- `hh` 是两位小写十六进制

例如：

- `string.result.Hash` -> `string_2eresult_2eHash`
- `Box[i32]` -> `Box_5bi32_5d`
- `ios/file` -> `ios_2ffile`

### 8.3 `exportNamespace` 什么时候打开

顶层 native 声明的最终链接名还受 `exportNamespace` 影响。

当前 `WorkspaceBuilder` 的规则是：

- dependency unit：`exportNamespace = true`
- 被当作 dependency 编译的 entry unit：`exportNamespace = true`
- standalone root entry：`exportNamespace = false`

因此：

- 作为可复用模块对外发射时，native 顶层声明通常带 canonical module prefix
- 单独编译入口模块时，顶层 native function / global 可能先保留裸 local name

这不是两套语义，而是“当前单模块产物是否需要对外导出模块命名空间”的差异。

### 8.4 顶层语义导出名

native 顶层函数 / 全局 / 类型，默认基于 canonical module path 导出。

这里的 canonical module path 有两条硬约束：

- 每个已加载模块都必须恰好属于一个模块 root
- 模块 root 集合是“root 文件所在目录 + 显式 `-I` roots”，并且这些 roots 不能重叠

例如：

- `string/result.lo`
- include root 为 `src`

则导出前缀是：

- `string.result`

对应：

- `Result` -> `string.result.Result`
- `Hash` -> `string.result.Hash`
- `make` -> `string.result.make`

需要注意：

- 类型和 trait 默认只有“语义导出名”
- 它们本身不是普通顶层 linker symbol
- 真正进入链接器的是函数、全局、方法、witness table、模块入口等实体

如果两个不同 roots 同时包含 `string/result.lo`，那这不是“前缀推导优先级问题”，而是配置错误；编译必须报模块冲突，而不是按 `-I` 顺序挑一个。

### 8.5 可导出符号命名规则总表

下面这张表描述“所有会进入 LLVM / linker 的主要符号”的当前规则。

| 类别 | 语义身份 / 来源 | 当前链接符号规则 | 备注 |
| --- | --- | --- | --- |
| 顶层 native function | `FunctionDecl.symbolName` | 规范导出名为 `<export-namespace>.<local>` | 依赖导出模式下使用这条规则 |
| 顶层 `#[extern "C"]` function | 顶层函数 | 直接用源码 local name | 不加模块前缀 |
| 顶层 native function，非导出模式 | 顶层函数 | 优先用裸 `<local>`；若与现有对象冲突则退回 `<export-namespace>.<local>` | 只影响当前 lowering 时的 runtime 名 |
| 顶层 non-extern global | `GlobalDecl.symbolName` | 规范导出名为 `<export-namespace>.<local>` | 依赖导出模式下使用这条规则 |
| 顶层 `extern` global | 顶层全局 | 直接用源码 local name | 不加模块前缀 |
| 顶层 global，非导出模式 | 顶层全局 | 当前 materialization 可用裸 `<local>` | 接口里仍保留 canonical `symbolName` |
| 非 generic struct inherent method | `StructType.full_name` + method local name | `<SelfTypeFullName>.<method>` | 例如 `string.result.Point.hash` |
| concrete applied struct 的非 generic inherent method | concrete applied `StructType.full_name` | `<mangle(SelfTypeFullName)>.<method>` | 避免 `[]`、`.` 等进入符号名 |
| trait impl method | concrete self type + trait exported name + method local name | `<SelfTypeFullName>.__trait__.<mangle(TraitExportedName)>.<method>` | generic / non-generic concrete impl 最终都收敛到这条规则 |
| generic function concrete instance | generic function declaration | `<base>__inst__<__mangle(typeArg1)>...` | `base` 来自函数当前 runtime name |
| generic struct method concrete instance | concrete self type + method + method type args | `<mangle(SelfTypeFullName)>.<method>[__inst__<__mangle(typeArg)>...]` | method 无额外 type arg 时没有 `__inst` 后缀 |
| trait witness table | `(Trait, ConcreteSelf)` | `__lona_trait_witness__<mangle(traitName)>__<mangle(selfTypeName)>` | `InternalLinkage` |
| 语言入口 | root language entry | `__lona_main__` | 语言级固定入口 |
| 模块 init entry | module key | `__<mangle(moduleKey)>_init_entry__` | 合成函数 |
| 模块 init state | module key | `__<mangle(moduleKey)>_init_state__` | 合成全局 |
| 模块 init result | module key | `__<mangle(moduleKey)>_init_result__` | 合成全局 |
| 字节串 helper global | 编译器内部常量池 | `.lona.global.bytes.<id>` | `PrivateLinkage`，不属于模块导出接口 |

### 8.6 顶层函数与全局的两层命名

顶层函数 / 全局最容易让人误解，因为它们同时有：

- `ModuleInterface` 里的 canonical `symbolName`
- 本轮 lowering 使用的 runtime 名

对于 native 顶层函数：

- `ModuleInterface::symbolName` 总是 canonical 导出名
- 但 `declareFunction(...)` 在 `exportNamespace == false` 时，可能先用裸 local name
- 如果裸 local name 与现有对象冲突，再回退到 canonical 导出名

对于顶层 global：

- `GlobalDecl.symbolName` 对 non-extern global 总是 canonical 导出名
- 但当前 unit interface materialization 在非导出模式下可先把当前模块内可见 runtime 名绑定成裸 local name

所以：

- “模块接口里记的名字”
- “当前这轮 LLVM lowering 实际采用的名字”

不一定永远完全相同。

### 8.7 方法前缀来自 `StructType.full_name`

method 系列的命名规则有一个统一源头：

- 先确定 concrete receiver 的 `StructType.full_name`
- 再在这个前缀后拼方法部分

因此：

- 普通 inherent method 的前缀来自 `StructType.full_name`
- trait impl method 的前缀也来自 `StructType.full_name`
- applied / generic concrete receiver 的 method 名是否需要先 `mangle`，也取决于 `StructType.full_name` 是否直接可安全放进 symbol

这也是为什么“方法符号名”不能脱离具体 `StructType` 单独讨论。

### 8.8 trait method symbol

trait impl method 的 runtime symbol 不是裸的 `Type.method`，而是：

```text
SelfType.__trait__.<mangled trait name>.method
```

这条规则让下面两项可以共存：

- `Point.hash` 这个 inherent method
- `Point.__trait__....Hash.hash` 这个 trait impl method

### 8.9 witness table symbol

trait object witness table 也有独立名字空间：

```text
__lona_trait_witness__<mangled trait>__<mangled self>
```

它不进入源码可见名字空间，也不进入顶层 lookup。

### 8.10 generic instance symbol只是结果，不是身份

generic function / method 的 runtime symbol 当前通常形如：

- `baseSymbol__inst__<type-args...>`
- `mangle(selfType).method__inst__<type-args...>`

但这只是发射名，不是 dedup identity。

当前必须继续保持：

- dedup 靠 `GenericInstanceKey`
- emitted symbol string 只是结果

不能反过来靠 symbol string 猜“是否是同一个实例”。

## 9. 一个具体例子

为了说明“符号表长什么样”，下面给一个缩略例子：

```lona
import dep/result

trait Hash {
    def hash() i32
}

struct Box[T] {
    value T
}

impl[T Hash] Hash for Box[T] {
    def hash() i32 {
        ret Hash.hash(&self.value)
    }
}

def wrap[T](value T) Box[T] {
    ret Box[T](value = value)
}
```

忽略部分字段后，各层大致会长成下面这样。

### 9.1 `ModuleInterface`

```text
localTraits_:
  "Hash" -> TraitDecl{
      localName = "Hash",
      exportedName = "<module>.Hash",
      methods = ["hash"]
  }

localTypes_:
  "Box" -> TypeDecl{
      localName = "Box",
      exportedName = "<module>.Box",
      typeParams = ["T"],
      methodTemplates = []
  }

localFunctions_:
  "wrap" -> FunctionDecl{
      localName = "wrap",
      symbolName = "<module>.wrap",
      typeParams = ["T"]
  }

traitImpls_:
  TraitImplDecl{
      selfTypeSpelling = "<module>.Box[T]"
      traitName = "<module>.Hash",
      typeParams = ["T" bound "<module>.Hash"],
      hasBody = true,
      bodyMethods = ["hash"]
  }
```

### 9.2 `CompilationUnit`

```text
localTraitBindings_:
  "Hash" -> "<module>.Hash"

localTypeBindings_:
  "Box" -> "<module>.Box"

localFunctionBindings_:
  "wrap" -> "<module>.wrap"

importedModules_:
  "result" -> ImportedModule{ ... }
```

注意这里仍然没有：

- `Box[i32]`
- `wrap[i32]`
- `Hash for Box[i32]`

这些都要到后面的类型表和 generic instance 表里才会出现。

### 9.3 `TypeTable` / `StructType`

当某处第一次真正需要 `Box[i32]` 时，才会出现近似这样的记录：

```text
typeMap:
  "Box[i32]" -> StructType(...)

StructType("Box[i32]"):
  members:
    "value" -> i32
  traitMethodTypes:
    "<module>.Hash::hash" -> FuncType(...)

methodFunctions_:
  (StructType("Box[i32]"), "<module>.Hash::hash") -> Function*
```

### 9.4 `GenericInstanceRegistry`

如果 `wrap[i32]` 或 `Box[i32]` 的 trait method 被真正实例化，还会有：

```text
GenericInstanceKey{
  ownerModuleKey = "<owner>"
  kind = Function / Method
  templateName = "<module>.Box" or "wrap" or "<trait> for <self>"
  methodName = "hash"
  concreteTypeArgs = ["i32"]
}
```

而 emitter 记录会单独指出：

```text
owners_[key] = {
  moduleKey = "<chosen emitter module>",
  symbolNames = ["..."]
}
```

## 10. 需要长期保持的工程约束

最后把最关键的约束单独列出来，后续改实现时应以这些为准。

1. 顶层声明表、局部词法表、类型表、generic runtime 表必须继续分层，不要重新揉成单张“万能符号表”。
2. 顶层 `trait impl` 继续只作为可见性/满足性记录存在，不进入顶层 lookup namespace。
3. 顶层类型与函数继续允许先使用后定义；局部绑定继续只按块级、从上到下生效。
4. direct import 才能贡献模块 alias；transitive import 不得注入当前模块顶层名字空间。
5. trait method 继续按 `TraitExportedName::methodLocalName` 建槽，而不是只按裸方法名建槽。
6. plain dot lookup 在看到多个 trait method 同名候选时，必须保持二义性诊断。
7. generic concrete instance 的身份继续由 structured key 决定，不能退回到“按 requester 分裂”或“按 mangled symbol 猜身份”。
8. runtime symbol name、trait slot key、source local name 必须继续视为三种不同概念，不能混用。

如果未来要扩 generic trait、associated type 或 multi-bound，这份文档应该先更新，再改实现。
