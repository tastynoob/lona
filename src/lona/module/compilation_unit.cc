#include "compilation_unit.hh"
#include "lona/ast/array_dim.hh"
#include "lona/ast/tag_apply.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/err/err.hh"
#include "lona/type/type.hh"
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string_view>
#include <utility>

namespace lona {
namespace {

std::string
deriveModuleName(const std::string &path) {
    namespace fs = std::filesystem;
    auto stem = fs::path(path).stem().string();
    if (!stem.empty()) {
        return stem;
    }
    return fs::path(path).filename().string();
}

std::string
deriveModuleKey(const std::string &path) {
    namespace fs = std::filesystem;
    auto normalized = fs::path(path).lexically_normal();
    normalized.replace_extension();
    auto key = normalized.string();
    if (!key.empty()) {
        return key;
    }
    return deriveModuleName(path);
}

constexpr std::uint64_t kHashSeed = 0x9e3779b97f4a7c15ULL;

std::uint64_t
combineHash(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + kHashSeed + (seed << 6) + (seed >> 2);
    return seed;
}

std::string
toStdString(const string &value) {
    return {value.tochara(), value.size()};
}

std::string
baseTypeName(const BaseTypeNode *node) {
    if (!node) {
        return {};
    }
    if (node->hasSyntax()) {
        return describeDotLikeSyntax(node->syntax);
    }
    return toStdString(node->name);
}

bool
splitBaseTypeName(const BaseTypeNode *node, std::string &moduleName,
                  std::string &memberName) {
    if (!node) {
        return false;
    }
    if (node->hasSyntax()) {
        std::vector<std::string> segments;
        if (!collectDotLikeSegments(node->syntax, segments) || segments.size() < 2) {
            return false;
        }

        moduleName = segments.front();
        memberName.clear();
        for (std::size_t i = 1; i < segments.size(); ++i) {
            if (!memberName.empty()) {
                memberName += ".";
            }
            memberName += segments[i];
        }
        return true;
    }
    auto rawName = baseTypeName(node);
    auto separator = rawName.find('.');
    if (separator == std::string::npos) {
        return false;
    }
    moduleName = rawName.substr(0, separator);
    memberName = rawName.substr(separator + 1);
    return true;
}

[[noreturn]] void
errorInvalidArrayDimension(const location &loc) {
    throw DiagnosticError(
        DiagnosticError::Category::Semantic, loc,
        "fixed-dimension arrays require positive integer literal sizes",
        "Use explicit sizes like `i32[4][5]` or `i32[5,4]`. Dimension inference and non-constant sizes are not implemented yet.");
}

[[noreturn]] void
errorUnsupportedUnsizedArray(const location &loc, const TypeNode *node) {
    throw DiagnosticError(
        DiagnosticError::Category::Semantic, loc,
        "explicit unsized array type syntax is not allowed: " +
            describeTypeNode(node, "<unknown type>"),
        "Use fixed explicit dimensions like `i32[2]`. If you want inferred array dimensions, write `var a = {1, 2}`. If you need an indexable pointer, write `T[*]`.");
}

[[noreturn]] void
errorLegacyIndexablePointerSyntax(const location &loc, const TypeNode *node) {
    throw DiagnosticError(
        DiagnosticError::Category::Semantic, loc,
        "explicit unsized array type syntax is not allowed inside pointer declarations: " +
            describeTypeNode(node, "<unknown type>"),
        "Use `T[*]` instead, for example `u8[*]`. `[]` is not a user-writable type declaration syntax.");
}

void
validateTypeNodeLayout(const TypeNode *node) {
    if (!node) {
        return;
    }
    if (auto *param = dynamic_cast<const FuncParamTypeNode *>(node)) {
        validateTypeNodeLayout(param->type);
        return;
    }
    if (auto *qualified = dynamic_cast<const ConstTypeNode *>(node)) {
        validateTypeNodeLayout(qualified->base);
        return;
    }
    if (auto *pointer = dynamic_cast<const PointerTypeNode *>(node)) {
        if (auto *array = dynamic_cast<const ArrayTypeNode *>(pointer->base);
            array && hasUnsizedArrayDimensions(array->dim) &&
            isBareUnsizedArraySyntax(array->dim)) {
            errorLegacyIndexablePointerSyntax(pointer->loc, pointer);
        }
        validateTypeNodeLayout(pointer->base);
        return;
    }
    if (auto *indexable = dynamic_cast<const IndexablePointerTypeNode *>(node)) {
        validateTypeNodeLayout(indexable->base);
        return;
    }
    if (auto *array = dynamic_cast<const ArrayTypeNode *>(node)) {
        validateTypeNodeLayout(array->base);
        if (hasUnsizedArrayDimensions(array->dim)) {
            errorUnsupportedUnsizedArray(array->loc, array);
        }
        for (auto *dimension : array->dim) {
            std::int64_t value = 0;
            if (!tryExtractArrayDimension(dimension, value) || value <= 0) {
                errorInvalidArrayDimension(dimension ? dimension->loc : array->loc);
            }
        }
        return;
    }
    if (auto *tuple = dynamic_cast<const TupleTypeNode *>(node)) {
        for (auto *item : tuple->items) {
            validateTypeNodeLayout(item);
        }
        return;
    }
    if (auto *func = dynamic_cast<const FuncPtrTypeNode *>(node)) {
        for (auto *arg : func->args) {
            validateTypeNodeLayout(arg);
        }
        validateTypeNodeLayout(func->ret);
        return;
    }
}

void
hashText(std::uint64_t &seed, std::string_view text) {
    seed = combineHash(seed, std::hash<std::string_view>{}(text));
}

void
hashTypeNode(std::uint64_t &seed, const TypeNode *node);

void
hashArrayDimensions(std::uint64_t &seed, const std::vector<AstNode *> &dimensions) {
    seed = combineHash(seed, dimensions.size());
    for (auto *dimension : dimensions) {
        if (dimension == nullptr) {
            hashText(seed, "array-dim:null");
            continue;
        }
        std::int64_t value = 0;
        if (tryExtractArrayDimension(dimension, value)) {
            hashText(seed, "array-dim:int");
            seed = combineHash(seed, static_cast<std::uint64_t>(value));
            continue;
        }
        hashText(seed, "array-dim:expr");
    }
}

void
hashParamSignature(std::uint64_t &seed, AstNode *node) {
    if (auto *decl = dynamic_cast<AstVarDecl *>(node)) {
        hashText(seed, "param");
        hashText(seed, bindingKindKeyword(decl->bindingKind));
        hashText(seed, toStdString(decl->field));
        hashTypeNode(seed, decl->typeNode);
        return;
    }
    if (auto *def = dynamic_cast<AstVarDef *>(node)) {
        hashText(seed, "param");
        hashText(seed, bindingKindKeyword(def->getBindingKind()));
        hashText(seed, def->isReadOnlyBinding() ? "readonly-binding" : "mutable-binding");
        hashText(seed, toStdString(def->getName()));
        hashTypeNode(seed, def->getTypeNode());
        return;
    }
    hashText(seed, "unknown-param");
}

void
hashInterfaceNode(std::uint64_t &seed, AstNode *node);

void
hashInterfaceList(std::uint64_t &seed, AstNode *node) {
    auto *list = dynamic_cast<AstStatList *>(node);
    if (!list) {
        hashInterfaceNode(seed, node);
        return;
    }
    hashText(seed, "list");
    for (auto *stmt : list->getBody()) {
        hashInterfaceNode(seed, stmt);
    }
}

void
hashInterfaceNode(std::uint64_t &seed, AstNode *node) {
    if (node == nullptr) {
        hashText(seed, "null");
        return;
    }
    if (auto *program = dynamic_cast<AstProgram *>(node)) {
        hashText(seed, "program");
        hashInterfaceList(seed, program->body);
        return;
    }
    if (auto *list = dynamic_cast<AstStatList *>(node)) {
        hashInterfaceList(seed, list);
        return;
    }
    if (auto *importNode = dynamic_cast<AstImport *>(node)) {
        (void)importNode;
        return;
    }
    if (auto *structDecl = dynamic_cast<AstStructDecl *>(node)) {
        hashText(seed, "struct");
        hashText(seed, toStdString(structDecl->name));
        hashText(seed, structDeclKindKeyword(structDecl->declKind));
        if (structDecl->body) {
            hashInterfaceList(seed, structDecl->body);
        } else {
            hashText(seed, "struct-body:none");
        }
        return;
    }
    if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(node)) {
        hashText(seed, "func");
        hashText(seed, toStdString(funcDecl->name));
        hashText(seed, abiKindKeyword(funcDecl->abiKind));
        hashText(seed, accessKindKeyword(funcDecl->receiverAccess));
        if (funcDecl->args) {
            seed = combineHash(seed, funcDecl->args->size());
            for (auto *arg : *funcDecl->args) {
                hashParamSignature(seed, arg);
            }
        } else {
            seed = combineHash(seed, 0);
        }
        hashTypeNode(seed, funcDecl->retType);
        hashText(seed, funcDecl->hasBody() ? "func-body:present"
                                           : "func-body:none");
        return;
    }
    if (auto *varDecl = dynamic_cast<AstVarDecl *>(node)) {
        hashText(seed, "field");
        hashText(seed, toStdString(varDecl->field));
        hashText(seed, accessKindKeyword(varDecl->accessKind));
        hashTypeNode(seed, varDecl->typeNode);
        return;
    }
    hashText(seed, "non-interface");
}

void
hashTypeNode(std::uint64_t &seed, const TypeNode *node) {
    if (node == nullptr) {
        hashText(seed, "void");
        return;
    }
    if (auto *param = dynamic_cast<const FuncParamTypeNode *>(node)) {
        hashText(seed, "func-param");
        hashText(seed, bindingKindKeyword(param->bindingKind));
        hashTypeNode(seed, param->type);
        return;
    }
    if (auto *qualified = dynamic_cast<const ConstTypeNode *>(node)) {
        hashText(seed, "const");
        hashTypeNode(seed, qualified->base);
        return;
    }
    if (auto *base = dynamic_cast<const BaseTypeNode *>(node)) {
        hashText(seed, "base");
        hashText(seed, baseTypeName(base));
        return;
    }
    if (auto *pointer = dynamic_cast<const PointerTypeNode *>(node)) {
        hashText(seed, "ptr");
        seed = combineHash(seed, pointer->dim);
        hashTypeNode(seed, pointer->base);
        return;
    }
    if (auto *indexable = dynamic_cast<const IndexablePointerTypeNode *>(node)) {
        hashText(seed, "indexable-ptr");
        hashTypeNode(seed, indexable->base);
        return;
    }
    if (auto *array = dynamic_cast<const ArrayTypeNode *>(node)) {
        hashText(seed, "array");
        hashArrayDimensions(seed, array->dim);
        hashTypeNode(seed, array->base);
        return;
    }
    if (auto *tuple = dynamic_cast<const TupleTypeNode *>(node)) {
        hashText(seed, "tuple");
        seed = combineHash(seed, tuple->items.size());
        for (auto *item : tuple->items) {
            hashTypeNode(seed, item);
        }
        return;
    }
    if (auto *func = dynamic_cast<const FuncPtrTypeNode *>(node)) {
        hashText(seed, "func-ptr-type");
        seed = combineHash(seed, func->args.size());
        for (auto *arg : func->args) {
            hashTypeNode(seed, arg);
        }
        hashTypeNode(seed, func->ret);
        return;
    }
    hashText(seed, "unknown-type");
}

std::uint64_t
computeInterfaceHash(AstNode *root) {
    std::uint64_t seed = kHashSeed;
    hashInterfaceNode(seed, root);
    return seed;
}

TypeClass *
resolveTypeNode(TypeTable *typeTable, const CompilationUnit &unit, TypeNode *node,
                bool validateLayout = true) {
    if (!typeTable || !node) {
        return nullptr;
    }

    if (validateLayout) {
        validateTypeNodeLayout(node);
    }

    if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
        return resolveTypeNode(typeTable, unit, param->type, false);
    }

    TypeClass *resolved = nullptr;

    if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
        auto *baseType = resolveTypeNode(typeTable, unit, qualified->base, false);
        resolved = baseType ? typeTable->createConstType(baseType) : nullptr;
        unit.cacheResolvedType(node, resolved);
        return resolved;
    }

    if (auto *cached = unit.findResolvedType(node)) {
        return cached;
    }
    if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
        auto rawName = baseTypeName(base);
        std::string moduleName;
        std::string memberName;
        if (!splitBaseTypeName(base, moduleName, memberName)) {
            auto lookup = unit.lookupTopLevelName(rawName);
            if (lookup.isType()) {
                auto *type = typeTable->getType(llvm::StringRef(lookup.resolvedName));
                unit.cacheResolvedType(node, type);
                return type;
            }
        } else {
            const auto *imported = unit.findImportedModule(moduleName);
            if (!imported) {
                unit.cacheResolvedType(node, nullptr);
                return nullptr;
            }
            auto lookup = unit.lookupTopLevelName(*imported, memberName);
            if (!lookup.isType()) {
                unit.cacheResolvedType(node, nullptr);
                return nullptr;
            }
        }
        resolved = typeTable->getType(llvm::StringRef(rawName));
        unit.cacheResolvedType(node, resolved);
        return resolved;
    }

    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        auto *type = resolveTypeNode(typeTable, unit, pointer->base, false);
        for (uint32_t i = 0; type && i < pointer->dim; ++i) {
            type = typeTable->createPointerType(type);
        }
        unit.cacheResolvedType(node, type);
        return type;
    }

    if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
        auto *elementType = resolveTypeNode(typeTable, unit, indexable->base, false);
        resolved = elementType ? typeTable->createIndexablePointerType(elementType)
                               : nullptr;
        unit.cacheResolvedType(node, resolved);
        return resolved;
    }

    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        auto *elementType = resolveTypeNode(typeTable, unit, array->base, false);
        if (!elementType) {
            return nullptr;
        }
        resolved = typeTable->createArrayType(elementType, array->dim);
        unit.cacheResolvedType(node, resolved);
        return resolved;
    }

    if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
        std::vector<TypeClass *> itemTypes;
        itemTypes.reserve(tuple->items.size());
        for (auto *item : tuple->items) {
            auto *itemType = resolveTypeNode(typeTable, unit, item, false);
            if (!itemType) {
                return nullptr;
            }
            itemTypes.push_back(itemType);
        }
        resolved = typeTable->getOrCreateTupleType(itemTypes);
        unit.cacheResolvedType(node, resolved);
        return resolved;
    }

    if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
        std::vector<TypeClass *> argTypes;
        std::vector<BindingKind> argBindingKinds;
        argTypes.reserve(func->args.size());
        argBindingKinds.reserve(func->args.size());
        for (auto *arg : func->args) {
            argBindingKinds.push_back(funcParamBindingKind(arg));
            auto *argType =
                resolveTypeNode(typeTable, unit, unwrapFuncParamType(arg), false);
            if (!argType) {
                return nullptr;
            }
            argTypes.push_back(argType);
        }
        auto *retType = resolveTypeNode(typeTable, unit, func->ret, false);
        auto *funcType = typeTable->getOrCreateFunctionType(
            argTypes, retType, std::move(argBindingKinds));
        resolved = funcType ? typeTable->createPointerType(funcType) : nullptr;
        unit.cacheResolvedType(node, resolved);
        return resolved;
    }

    return nullptr;
}

}  // namespace

CompilationUnit::CompilationUnit(const SourceBuffer &source) {
    refreshSource(source);
}

const SourceBuffer &
CompilationUnit::source() const {
    assert(source_ != nullptr);
    return *source_;
}

std::uint64_t
CompilationUnit::sourceHash() const {
    return moduleInterface_ ? moduleInterface_->sourceHash()
                            : hashModuleSource(source().content());
}

std::uint64_t
CompilationUnit::interfaceHash() const {
    ensureHashes();
    return interfaceHash_;
}

std::uint64_t
CompilationUnit::implementationHash() const {
    ensureHashes();
    return implementationHash_;
}

void
CompilationUnit::attachInterface(std::shared_ptr<ModuleInterface> moduleInterface) {
    moduleInterface_ = std::move(moduleInterface);
    if (moduleInterface_) {
        moduleInterface_->refresh(path_, moduleKey_, moduleName_,
                                  hashModuleSource(source().content()));
    }
}

void
CompilationUnit::refreshSource(const SourceBuffer &source) {
    const auto newPath = source.path();
    const auto newKey = deriveModuleKey(newPath);
    const auto newName = deriveModuleName(newPath);
    const auto newHash = hashModuleSource(source.content());
    const bool changed = !source_ || path_ != newPath || !moduleInterface_ ||
        moduleInterface_->sourceHash() != newHash;

    path_ = source.path();
    moduleKey_ = std::move(newKey);
    moduleName_ = std::move(newName);
    source_ = &source;
    if (moduleInterface_) {
        moduleInterface_->refresh(path_, moduleKey_, moduleName_, newHash);
    }
    if (changed) {
        syntaxTree_ = nullptr;
        stage_ = CompilationUnitStage::Discovered;
        clearImportedModules();
        clearLocalBindings();
        invalidateCaches();
        clearInterface();
    }
}

void
CompilationUnit::setSyntaxTree(AstNode *tree) {
    syntaxTree_ = applyBuiltinTags(tree);
    validateBuiltinTagResults(syntaxTree_);
    invalidateCaches();
    stage_ = tree ? CompilationUnitStage::Parsed : CompilationUnitStage::Discovered;
}

void
CompilationUnit::markDependenciesScanned() {
    if (syntaxTree_ != nullptr) {
        stage_ = CompilationUnitStage::DependenciesScanned;
    }
}

void
CompilationUnit::markInterfaceCollected() {
    if (moduleInterface_) {
        moduleInterface_->markCollected();
    }
    if (syntaxTree_ != nullptr) {
        stage_ = CompilationUnitStage::InterfaceCollected;
    }
}

void
CompilationUnit::markCompiled() {
    if (syntaxTree_ != nullptr) {
        stage_ = CompilationUnitStage::Compiled;
    }
}

void
CompilationUnit::clearImportedModules() {
    importedModules_.clear();
}

void
CompilationUnit::clearLocalBindings() {
    localTypeBindings_.clear();
    localFunctionBindings_.clear();
}

bool
CompilationUnit::addImportedModule(std::string alias, const CompilationUnit &unit) {
    ImportedModule imported = {unit.path(), unit.moduleKey(), unit.moduleName(),
                               unit.interface()};
    return importedModules_.emplace(std::move(alias), std::move(imported)).second;
}

const CompilationUnit::ImportedModule *
CompilationUnit::findImportedModule(const std::string &alias) const {
    auto found = importedModules_.find(alias);
    if (found == importedModules_.end()) {
        return nullptr;
    }
    return &found->second;
}

bool
CompilationUnit::importsModule(const std::string &alias) const {
    return findImportedModule(alias) != nullptr;
}

CompilationUnit::TopLevelLookup
CompilationUnit::lookupTopLevelName(const std::string &name) const {
    TopLevelLookup lookup;

    if (const auto *typeName = findLocalType(name)) {
        lookup.kind = TopLevelLookupKind::Type;
        lookup.resolvedName = *typeName;
        if (moduleInterface_) {
            lookup.typeDecl = moduleInterface_->findType(name);
        }
        return lookup;
    }

    if (const auto *functionName = findLocalFunction(name)) {
        lookup.kind = TopLevelLookupKind::Function;
        lookup.resolvedName = *functionName;
        if (moduleInterface_) {
            lookup.functionDecl = moduleInterface_->findFunction(name);
        }
        return lookup;
    }

    if (const auto *imported = findImportedModule(name)) {
        lookup.kind = TopLevelLookupKind::Module;
        lookup.importedModule = imported;
        lookup.resolvedName = imported->moduleName;
        return lookup;
    }

    return lookup;
}

CompilationUnit::TopLevelLookup
CompilationUnit::lookupTopLevelName(const ImportedModule &moduleNamespace,
                                    const std::string &name) const {
    TopLevelLookup lookup;
    if (!moduleNamespace.interface) {
        return lookup;
    }

    auto member = moduleNamespace.interface->lookupTopLevelName(name);
    if (member.isType()) {
        lookup.kind = TopLevelLookupKind::Type;
        lookup.importedModule = &moduleNamespace;
        lookup.typeDecl = member.typeDecl;
        lookup.resolvedName = member.typeDecl ? member.typeDecl->exportedName : std::string();
        return lookup;
    }
    if (member.isFunction()) {
        lookup.kind = TopLevelLookupKind::Function;
        lookup.importedModule = &moduleNamespace;
        lookup.functionDecl = member.functionDecl;
        lookup.resolvedName =
            member.functionDecl ? member.functionDecl->symbolName : std::string();
        return lookup;
    }
    return lookup;
}

void
CompilationUnit::clearInterface() {
    if (moduleInterface_) {
        moduleInterface_->clear();
    }
    clearLocalBindings();
    invalidateCaches();
    resolvedTypes_.clear();
}

void
CompilationUnit::invalidateCaches() {
    hashesReady_ = false;
    interfaceHash_ = 0;
    implementationHash_ = 0;
}

void
CompilationUnit::ensureHashes() const {
    if (hashesReady_) {
        return;
    }
    interfaceHash_ = syntaxTree_ ? computeInterfaceHash(syntaxTree_) : 0;
    implementationHash_ = sourceHash();
    hashesReady_ = true;
}

bool
CompilationUnit::bindLocalType(std::string localName, std::string resolvedName) {
    return localTypeBindings_
        .emplace(std::move(localName), std::move(resolvedName))
        .second;
}

bool
CompilationUnit::bindLocalFunction(std::string localName, std::string resolvedName) {
    return localFunctionBindings_
        .emplace(std::move(localName), std::move(resolvedName))
        .second;
}

const std::string *
CompilationUnit::findLocalType(const std::string &localName) const {
    auto found = localTypeBindings_.find(localName);
    if (found == localTypeBindings_.end()) {
        return nullptr;
    }
    return &found->second;
}

const std::string *
CompilationUnit::findLocalFunction(const std::string &localName) const {
    auto found = localFunctionBindings_.find(localName);
    if (found == localFunctionBindings_.end()) {
        return nullptr;
    }
    return &found->second;
}

TypeClass *
CompilationUnit::findResolvedType(TypeNode *node) const {
    auto found = resolvedTypes_.find(node);
    if (found == resolvedTypes_.end()) {
        return nullptr;
    }
    return found->second;
}

void
CompilationUnit::cacheResolvedType(TypeNode *node, TypeClass *type) const {
    if (node) {
        resolvedTypes_[node] = type;
    }
}

void
CompilationUnit::clearResolvedTypes() {
    resolvedTypes_.clear();
}

TypeClass *
CompilationUnit::resolveType(TypeTable *typeTable, TypeNode *node) const {
    return resolveTypeNode(typeTable, *this, node);
}

}  // namespace lona
