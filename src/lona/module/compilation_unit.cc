#include "compilation_unit.hh"
#include "lona/ast/tag_apply.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/declare/support.hh"
#include "lona/err/err.hh"
#include "lona/sema/initializer.hh"
#include "lona/type/type.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <tuple>
#include <string_view>
#include <utility>

namespace lona {

namespace compilation_unit_impl {

TypeClass *
resolveTypeNode(TypeTable *typeTable, const CompilationUnit &unit,
                TypeNode *node, bool validateLayout);

std::uint64_t
combineHash(std::uint64_t seed, std::uint64_t value);

void
hashText(std::uint64_t &seed, std::string_view text);

void
hashTypeParams(std::uint64_t &seed,
               const std::vector<AstGenericParam *> *typeParams) {
    if (!typeParams) {
        seed = combineHash(seed, 0);
        return;
    }
    seed = combineHash(seed, typeParams->size());
    for (auto *param : *typeParams) {
        if (!param) {
            hashText(seed, "<null>");
            continue;
        }
        hashText(seed, toStdString(param->name.text));
        hashText(seed, param->hasBoundTrait()
                           ? describeDotLikeSyntax(param->boundTrait, "<trait>")
                           : std::string());
    }
}

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

void
hashText(std::uint64_t &seed, std::string_view text) {
    seed = combineHash(seed, std::hash<std::string_view>{}(text));
}

void
hashTypeNode(std::uint64_t &seed, const TypeNode *node);

void
hashArrayDimensions(std::uint64_t &seed,
                    const std::vector<AstNode *> &dimensions) {
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
        hashText(seed, def->isReadOnlyBinding() ? "readonly-binding"
                                                : "mutable-binding");
        hashText(seed, toStdString(def->getName()));
        hashTypeNode(seed, def->getTypeNode());
        return;
    }
    hashText(seed, "unknown-param");
}

void
hashInferredGlobalType(std::uint64_t &seed, const AstNode *node) {
    if (node == nullptr) {
        hashText(seed, "global-infer:null");
        return;
    }
    if (auto *constant = dynamic_cast<const AstConst *>(node)) {
        hashText(seed, "global-infer:const");
        switch (constant->getType()) {
            case AstConst::Type::I8:
                hashText(seed, "i8");
                return;
            case AstConst::Type::U8:
            case AstConst::Type::CHAR:
                hashText(seed, "u8");
                return;
            case AstConst::Type::I16:
                hashText(seed, "i16");
                return;
            case AstConst::Type::U16:
                hashText(seed, "u16");
                return;
            case AstConst::Type::I32:
                hashText(seed, "i32");
                return;
            case AstConst::Type::U32:
                hashText(seed, "u32");
                return;
            case AstConst::Type::I64:
                hashText(seed, "i64");
                return;
            case AstConst::Type::U64:
                hashText(seed, "u64");
                return;
            case AstConst::Type::USIZE:
                hashText(seed, "usize");
                return;
            case AstConst::Type::F32:
                hashText(seed, "f32");
                return;
            case AstConst::Type::F64:
                hashText(seed, "f64");
                return;
            case AstConst::Type::BOOL:
                hashText(seed, "bool");
                return;
            case AstConst::Type::STRING:
                hashText(seed, "indexable-ptr");
                hashText(seed, "const");
                hashText(seed, "u8");
                return;
            case AstConst::Type::NULLPTR:
                hashText(seed, "null-pointer-untyped");
                return;
        }
    }
    if (auto *unary = dynamic_cast<const AstUnaryOper *>(node)) {
        if (unary->op == '+' || unary->op == '-') {
            hashText(seed, "global-infer:signed-unary");
            hashInferredGlobalType(seed, unary->expr);
            return;
        }
    }
    if (dynamic_cast<const AstBraceInit *>(node)) {
        hashText(seed, "global-infer:brace");
        return;
    }
    hashText(seed, "global-infer:unsupported");
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
        hashText(seed, "import");
        hashText(seed, importNode->path);
        return;
    }
    if (auto *structDecl = dynamic_cast<AstStructDecl *>(node)) {
        hashText(seed, "struct");
        hashText(seed, toStdString(structDecl->name));
        hashText(seed, structDeclKindKeyword(structDecl->declKind));
        hashTypeParams(seed, structDecl->typeParams);
        if (structDecl->body) {
            hashInterfaceList(seed, structDecl->body);
        } else {
            hashText(seed, "struct-body:none");
        }
        return;
    }
    if (auto *traitDecl = dynamic_cast<AstTraitDecl *>(node)) {
        hashText(seed, "trait");
        hashText(seed, toStdString(traitDecl->name));
        if (traitDecl->body) {
            hashInterfaceList(seed, traitDecl->body);
        } else {
            hashText(seed, "trait-body:none");
        }
        return;
    }
    if (auto *traitImplDecl = dynamic_cast<AstTraitImplDecl *>(node)) {
        hashText(seed, "trait-impl");
        hashTypeParams(seed, traitImplDecl->typeParams);
        hashTypeNode(seed, traitImplDecl->selfType);
        hashText(seed, describeDotLikeSyntax(traitImplDecl->trait));
        hashText(seed, traitImplDecl->hasBody() ? "impl-body:present"
                                                : "impl-body:none");
        return;
    }
    if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(node)) {
        hashText(seed, "func");
        hashText(seed, toStdString(funcDecl->name));
        hashText(seed, abiKindKeyword(funcDecl->abiKind));
        hashText(seed, accessKindKeyword(funcDecl->receiverAccess));
        hashTypeParams(seed, funcDecl->typeParams);
        if (funcDecl->args) {
            seed = combineHash(seed, funcDecl->args->size());
            for (auto *arg : *funcDecl->args) {
                hashParamSignature(seed, arg);
            }
        } else {
            seed = combineHash(seed, 0);
        }
        hashTypeNode(seed, funcDecl->retType);
        hashText(seed,
                 funcDecl->hasBody() ? "func-body:present" : "func-body:none");
        return;
    }
    if (auto *globalDecl = dynamic_cast<AstGlobalDecl *>(node)) {
        hashText(seed, "global");
        hashText(seed, toStdString(globalDecl->getName()));
        hashText(seed, globalDecl->isExtern() ? "extern" : "native");
        if (globalDecl->hasTypeNode()) {
            hashTypeNode(seed, globalDecl->getTypeNode());
        } else {
            hashInferredGlobalType(seed, globalDecl->getInitVal());
        }
        hashText(seed, globalDecl->hasInitVal() ? "global-init:present"
                                                : "global-init:none");
        return;
    }
    if (auto *varDecl = dynamic_cast<AstVarDecl *>(node)) {
        hashText(seed, "field");
        hashText(seed, toStdString(varDecl->field));
        hashText(seed, accessKindKeyword(varDecl->accessKind));
        hashText(seed, varDecl->isEmbeddedField() ? "embedded" : "plain");
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
    if (dynamic_cast<const AnyTypeNode *>(node)) {
        hashText(seed, "any");
        return;
    }
    if (auto *applied = dynamic_cast<const AppliedTypeNode *>(node)) {
        hashText(seed, "applied");
        hashTypeNode(seed, applied->base);
        seed = combineHash(seed, applied->args.size());
        for (auto *arg : applied->args) {
            hashTypeNode(seed, arg);
        }
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
    if (auto *dynType = dynamic_cast<const DynTypeNode *>(node)) {
        hashText(seed, "dyn");
        hashTypeNode(seed, dynType->base);
        return;
    }
    if (auto *pointer = dynamic_cast<const PointerTypeNode *>(node)) {
        hashText(seed, "ptr");
        seed = combineHash(seed, pointer->dim);
        hashTypeNode(seed, pointer->base);
        return;
    }
    if (auto *indexable =
            dynamic_cast<const IndexablePointerTypeNode *>(node)) {
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

std::string
genericTemplateHint(const std::string &rawName) {
    return "Write `" + rawName +
           "[...]` with explicit type arguments, or keep the template name "
           "inside another applied type like `" + rawName + "[T]`.";
}

[[noreturn]] void
errorBareGenericTemplateType(const location &loc, const std::string &rawName) {
    error(loc,
          "generic type template `" + rawName +
              "` requires explicit `[...]` type arguments",
          genericTemplateHint(rawName));
}

std::string
buildAppliedTypeName(const std::string &baseName,
                     const std::vector<TypeClass *> &args) {
    std::string name = baseName.empty() ? std::string("<type>") : baseName;
    name += "[";
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            name += ", ";
        }
        name += args[i] ? describeResolvedType(args[i]) : "<unknown type>";
    }
    name += "]";
    return name;
}

const ModuleInterface::TypeDecl *
resolveVisibleTypeDecl(const CompilationUnit &unit, BaseTypeNode *base) {
    if (!base) {
        return nullptr;
    }
    auto rawName = baseTypeName(base);
    std::string moduleName;
    std::string memberName;
    if (!splitBaseTypeName(base, moduleName, memberName)) {
        auto lookup = unit.lookupTopLevelName(rawName);
        return lookup.isType() ? lookup.typeDecl : nullptr;
    }

    const auto *imported = unit.findImportedModule(moduleName);
    if (!imported || !imported->interface) {
        return nullptr;
    }
    auto lookup = unit.lookupTopLevelName(*imported, memberName);
    return lookup.isType() ? lookup.typeDecl : nullptr;
}

const CompilationUnit *
findTemplateOwnerUnit(const CompilationUnit &contextUnit,
                      const ModuleInterface::TypeDecl &typeDecl) {
    if (contextUnit.ownsTypeDecl(&typeDecl)) {
        return &contextUnit;
    }
    for (const auto &entry : contextUnit.importedModules()) {
        const auto &imported = entry.second;
        if (!imported.unit || !imported.interface) {
            continue;
        }
        if (imported.interface->findType(toStdString(typeDecl.localName)) ==
            &typeDecl) {
            return imported.unit;
        }
    }
    return nullptr;
}

std::uint64_t
computeVisibleImportInterfaceHash(const CompilationUnit &unit) {
    std::uint64_t seed = kHashSeed;
    std::vector<std::pair<std::string, const CompilationUnit::ImportedModule *>>
        imports;
    imports.reserve(unit.importedModules().size());
    for (const auto &entry : unit.importedModules()) {
        imports.push_back({toStdString(entry.first), &entry.second});
    }
    std::sort(imports.begin(), imports.end(),
              [](const auto &lhs, const auto &rhs) {
                  return lhs.first < rhs.first;
              });
    seed = combineHash(seed, imports.size());
    for (const auto &[alias, imported] : imports) {
        hashText(seed, alias);
        if (!imported) {
            continue;
        }
        hashText(seed, toStdString(imported->moduleKey));
        hashText(seed, toStdString(imported->path));
        seed = combineHash(
            seed,
            imported->unit ? imported->unit->interfaceHash()
                           : (imported->interface ? imported->interface->sourceHash()
                                                  : 0));
    }
    return seed;
}

std::uint64_t
computeVisibleTraitImplHash(const CompilationUnit &unit) {
    struct ImplDescriptor {
        std::string sourceModuleKey;
        std::string traitName;
        std::string selfTypeSpelling;
        std::vector<std::pair<std::string, std::string>> typeParams;
    };

    std::uint64_t seed = kHashSeed;
    std::vector<ImplDescriptor> impls;
    if (const auto *interface = unit.interface()) {
        for (const auto &implDecl : interface->traitImpls()) {
            ImplDescriptor desc;
            desc.sourceModuleKey = toStdString(unit.moduleKey());
            desc.traitName = toStdString(implDecl.traitName);
            desc.selfTypeSpelling = toStdString(implDecl.selfTypeSpelling);
            for (const auto &param : implDecl.typeParams) {
                desc.typeParams.emplace_back(toStdString(param.localName),
                                             toStdString(param.boundTraitName));
            }
            impls.push_back(std::move(desc));
        }
    }
    for (const auto &entry : unit.importedModules()) {
        const auto &imported = entry.second;
        if (!imported.interface) {
            continue;
        }
        for (const auto &implDecl : imported.interface->traitImpls()) {
            ImplDescriptor desc;
            desc.sourceModuleKey = toStdString(imported.moduleKey);
            desc.traitName = toStdString(implDecl.traitName);
            desc.selfTypeSpelling = toStdString(implDecl.selfTypeSpelling);
            for (const auto &param : implDecl.typeParams) {
                desc.typeParams.emplace_back(toStdString(param.localName),
                                             toStdString(param.boundTraitName));
            }
            impls.push_back(std::move(desc));
        }
    }

    std::sort(impls.begin(), impls.end(), [](const ImplDescriptor &lhs,
                                             const ImplDescriptor &rhs) {
        return std::tie(lhs.sourceModuleKey, lhs.traitName, lhs.selfTypeSpelling,
                        lhs.typeParams) <
               std::tie(rhs.sourceModuleKey, rhs.traitName, rhs.selfTypeSpelling,
                        rhs.typeParams);
    });

    seed = combineHash(seed, impls.size());
    for (const auto &impl : impls) {
        hashText(seed, impl.sourceModuleKey);
        hashText(seed, impl.traitName);
        hashText(seed, impl.selfTypeSpelling);
        seed = combineHash(seed, impl.typeParams.size());
        for (const auto &[name, bound] : impl.typeParams) {
            hashText(seed, name);
            hashText(seed, bound);
        }
    }
    return seed;
}

bool
isGenericImplParam(
    const std::vector<ModuleInterface::GenericParamDecl> &typeParams,
    llvm::StringRef name) {
    for (const auto &param : typeParams) {
        if (toStringRef(param.localName) == name) {
            return true;
        }
    }
    return false;
}

std::string
canonicalTypePatternSpelling(
    const CompilationUnit &ownerUnit, const TypeNode *node,
    const std::unordered_map<std::string, TypeClass *> &genericBindings) {
    if (!node) {
        return "void";
    }
    if (auto *param = dynamic_cast<const FuncParamTypeNode *>(node)) {
        return canonicalTypePatternSpelling(ownerUnit, param->type,
                                            genericBindings);
    }
    if (dynamic_cast<const AnyTypeNode *>(node)) {
        return "any";
    }
    if (auto *base = dynamic_cast<const BaseTypeNode *>(node)) {
        auto rawName = baseTypeName(base);
        if (auto found = genericBindings.find(rawName);
            found != genericBindings.end() && found->second) {
            return toStdString(found->second->full_name);
        }
        std::string moduleName;
        std::string memberName;
        if (!splitBaseTypeName(base, moduleName, memberName)) {
            if (const auto *typeDecl =
                    resolveVisibleTypeDecl(ownerUnit, const_cast<BaseTypeNode *>(base))) {
                return toStdString(typeDecl->exportedName);
            }
            return rawName;
        }
        if (const auto *imported = ownerUnit.findImportedModule(moduleName)) {
            auto lookup = ownerUnit.lookupTopLevelName(*imported, memberName);
            if (lookup.isType() && lookup.typeDecl) {
                return toStdString(lookup.typeDecl->exportedName);
            }
        }
        return rawName;
    }
    if (auto *applied = dynamic_cast<const AppliedTypeNode *>(node)) {
        std::string text =
            canonicalTypePatternSpelling(ownerUnit, applied->base,
                                         genericBindings) +
            "[";
        for (std::size_t i = 0; i < applied->args.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            text += canonicalTypePatternSpelling(ownerUnit, applied->args[i],
                                                 genericBindings);
        }
        text += "]";
        return text;
    }
    if (auto *qualified = dynamic_cast<const ConstTypeNode *>(node)) {
        return canonicalTypePatternSpelling(ownerUnit, qualified->base,
                                            genericBindings) +
               " const";
    }
    if (auto *pointer = dynamic_cast<const PointerTypeNode *>(node)) {
        auto text =
            canonicalTypePatternSpelling(ownerUnit, pointer->base,
                                         genericBindings);
        for (uint32_t i = 0; i < pointer->dim; ++i) {
            text += "*";
        }
        return text;
    }
    if (auto *indexable = dynamic_cast<const IndexablePointerTypeNode *>(node)) {
        return canonicalTypePatternSpelling(ownerUnit, indexable->base,
                                            genericBindings) +
               "[*]";
    }
    if (auto *array = dynamic_cast<const ArrayTypeNode *>(node)) {
        return canonicalTypePatternSpelling(ownerUnit, array->base,
                                            genericBindings) +
               "[]";
    }
    if (auto *tuple = dynamic_cast<const TupleTypeNode *>(node)) {
        std::string text = "<";
        for (std::size_t i = 0; i < tuple->items.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            text += canonicalTypePatternSpelling(ownerUnit, tuple->items[i],
                                                 genericBindings);
        }
        text += ">";
        return text;
    }
    if (auto *func = dynamic_cast<const FuncPtrTypeNode *>(node)) {
        std::string text = "(";
        for (std::size_t i = 0; i < func->args.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            if (funcParamBindingKind(func->args[i]) == BindingKind::Ref) {
                text += "ref ";
            }
            text += canonicalTypePatternSpelling(ownerUnit,
                                                 unwrapFuncParamType(func->args[i]),
                                                 genericBindings);
        }
        text += ":";
        if (func->ret) {
            text += " ";
            text += canonicalTypePatternSpelling(ownerUnit, func->ret,
                                                 genericBindings);
        }
        text += ")";
        return text;
    }
    if (auto *dynType = dynamic_cast<const DynTypeNode *>(node)) {
        return canonicalTypePatternSpelling(ownerUnit, dynType->base,
                                            genericBindings) +
               " dyn";
    }
    return describeTypeNode(node, "void");
}

bool
matchTraitImplSelfTypePattern(
    const CompilationUnit &ownerUnit,
    const std::vector<ModuleInterface::GenericParamDecl> &typeParams,
    const TypeNode *pattern, TypeClass *actualType,
    std::unordered_map<std::string, TypeClass *> &genericBindings) {
    if (!pattern || !actualType) {
        return false;
    }
    if (auto *param = dynamic_cast<const FuncParamTypeNode *>(pattern)) {
        return matchTraitImplSelfTypePattern(ownerUnit, typeParams, param->type,
                                             actualType, genericBindings);
    }
    if (auto *base = dynamic_cast<const BaseTypeNode *>(pattern)) {
        auto rawName = baseTypeName(base);
        std::string moduleName;
        std::string memberName;
        if (!splitBaseTypeName(base, moduleName, memberName) &&
            isGenericImplParam(typeParams, rawName)) {
            auto found = genericBindings.find(rawName);
            if (found == genericBindings.end()) {
                genericBindings.emplace(rawName, actualType);
                return true;
            }
            return found->second == actualType;
        }
        return canonicalTypePatternSpelling(ownerUnit, pattern,
                                            genericBindings) ==
               toStdString(actualType->full_name);
    }
    if (auto *applied = dynamic_cast<const AppliedTypeNode *>(pattern)) {
        auto *actualStruct = asUnqualified<StructType>(actualType);
        if (!actualStruct || !actualStruct->isAppliedTemplateInstance()) {
            return false;
        }
        auto patternBaseName = canonicalTypePatternSpelling(
            ownerUnit, applied->base, genericBindings);
        if (toStdString(actualStruct->getAppliedTemplateName()) !=
            patternBaseName) {
            return false;
        }
        const auto &actualArgs = actualStruct->getAppliedTypeArgs();
        if (actualArgs.size() != applied->args.size()) {
            return false;
        }
        for (std::size_t i = 0; i < applied->args.size(); ++i) {
            if (!matchTraitImplSelfTypePattern(ownerUnit, typeParams,
                                               applied->args[i], actualArgs[i],
                                               genericBindings)) {
                return false;
            }
        }
        return true;
    }
    if (auto *qualified = dynamic_cast<const ConstTypeNode *>(pattern)) {
        auto *actualConst = actualType->as<ConstType>();
        return actualConst &&
               matchTraitImplSelfTypePattern(ownerUnit, typeParams,
                                             qualified->base,
                                             actualConst->getBaseType(),
                                             genericBindings);
    }
    if (auto *pointer = dynamic_cast<const PointerTypeNode *>(pattern)) {
        auto *current = actualType;
        for (uint32_t i = 0; i < pointer->dim; ++i) {
            auto *actualPointer = current ? current->as<PointerType>() : nullptr;
            if (!actualPointer) {
                return false;
            }
            current = actualPointer->getPointeeType();
        }
        return matchTraitImplSelfTypePattern(ownerUnit, typeParams,
                                             pointer->base, current,
                                             genericBindings);
    }
    if (auto *indexable = dynamic_cast<const IndexablePointerTypeNode *>(pattern)) {
        auto *actualIndexable = actualType->as<IndexablePointerType>();
        return actualIndexable &&
               matchTraitImplSelfTypePattern(ownerUnit, typeParams,
                                             indexable->base,
                                             actualIndexable->getElementType(),
                                             genericBindings);
    }
    if (auto *array = dynamic_cast<const ArrayTypeNode *>(pattern)) {
        auto *actualArray = actualType->as<ArrayType>();
        return actualArray &&
               matchTraitImplSelfTypePattern(ownerUnit, typeParams, array->base,
                                             actualArray->getElementType(),
                                             genericBindings);
    }
    if (auto *tuple = dynamic_cast<const TupleTypeNode *>(pattern)) {
        auto *actualTuple = actualType->as<TupleType>();
        if (!actualTuple ||
            actualTuple->getItemTypes().size() != tuple->items.size()) {
            return false;
        }
        for (std::size_t i = 0; i < tuple->items.size(); ++i) {
            if (!matchTraitImplSelfTypePattern(ownerUnit, typeParams,
                                               tuple->items[i],
                                               actualTuple->getItemTypes()[i],
                                               genericBindings)) {
                return false;
            }
        }
        return true;
    }
    if (auto *func = dynamic_cast<const FuncPtrTypeNode *>(pattern)) {
        auto *actualPointer = actualType->as<PointerType>();
        auto *actualFunc =
            actualPointer ? actualPointer->getPointeeType()->as<FuncType>()
                          : nullptr;
        if (!actualFunc ||
            actualFunc->getArgTypes().size() != func->args.size()) {
            return false;
        }
        for (std::size_t i = 0; i < func->args.size(); ++i) {
            if (!matchTraitImplSelfTypePattern(ownerUnit, typeParams,
                                               unwrapFuncParamType(func->args[i]),
                                               actualFunc->getArgTypes()[i],
                                               genericBindings)) {
                return false;
            }
        }
        return matchTraitImplSelfTypePattern(ownerUnit, typeParams, func->ret,
                                             actualFunc->getRetType(),
                                             genericBindings);
    }
    if (auto *dynType = dynamic_cast<const DynTypeNode *>(pattern)) {
        auto *actualDyn = actualType->as<DynTraitType>();
        return actualDyn &&
               canonicalTypePatternSpelling(ownerUnit, pattern,
                                            genericBindings) ==
                   toStdString(actualDyn->full_name);
    }
    return false;
}

bool
typeSatisfiesVisibleTraitImpl(
    const CompilationUnit &requesterUnit, const ::string &traitName,
    TypeClass *selfType, std::unordered_set<std::string> &active);

bool
traitImplMatchesConcreteSelfType(
    const CompilationUnit &requesterUnit, const CompilationUnit &ownerUnit,
    const ModuleInterface::TraitImplDecl &implDecl, TypeClass *selfType,
    std::unordered_set<std::string> &active) {
    if (!selfType) {
        return false;
    }
    std::unordered_map<std::string, TypeClass *> genericBindings;
    if (!implDecl.typeParams.empty()) {
        if (!matchTraitImplSelfTypePattern(ownerUnit, implDecl.typeParams,
                                           implDecl.selfTypeNode, selfType,
                                           genericBindings)) {
            return false;
        }
        for (const auto &param : implDecl.typeParams) {
            if (param.boundTraitName.empty()) {
                continue;
            }
            auto found = genericBindings.find(toStdString(param.localName));
            if (found == genericBindings.end() || !found->second) {
                return false;
            }
            if (!typeSatisfiesVisibleTraitImpl(requesterUnit,
                                               param.boundTraitName,
                                               found->second, active)) {
                return false;
            }
        }
        return true;
    }
    if (!implDecl.selfTypeNode) {
        return implDecl.selfTypeSpelling == selfType->full_name;
    }
    return matchTraitImplSelfTypePattern(ownerUnit, implDecl.typeParams,
                                         implDecl.selfTypeNode, selfType,
                                         genericBindings);
}

bool
typeSatisfiesVisibleTraitImpl(
    const CompilationUnit &requesterUnit, const ::string &traitName,
    TypeClass *selfType, std::unordered_set<std::string> &active) {
    if (!selfType) {
        return false;
    }
    auto key = toStdString(traitName) + "|" + toStdString(selfType->full_name);
    auto [_, inserted] = active.insert(key);
    if (!inserted) {
        return false;
    }
    struct Guard {
        std::unordered_set<std::string> &active;
        std::string key;
        ~Guard() { active.erase(key); }
    } guard{active, key};

    if (const auto *interface = requesterUnit.interface()) {
        for (const auto &implDecl : interface->traitImpls()) {
            if (implDecl.traitName != traitName) {
                continue;
            }
            if (traitImplMatchesConcreteSelfType(requesterUnit, requesterUnit,
                                                 implDecl, selfType, active)) {
                return true;
            }
        }
    }

    for (const auto &entry : requesterUnit.importedModules()) {
        const auto &imported = entry.second;
        if (!imported.interface || !imported.unit) {
            continue;
        }
        for (const auto &implDecl : imported.interface->traitImpls()) {
            if (implDecl.traitName != traitName) {
                continue;
            }
            if (traitImplMatchesConcreteSelfType(requesterUnit, *imported.unit,
                                                 implDecl, selfType, active)) {
                return true;
            }
        }
    }

    return false;
}

std::vector<string>
buildConcreteTypeArgNames(const std::vector<TypeClass *> &appliedTypeArgs) {
    std::vector<string> names;
    names.reserve(appliedTypeArgs.size());
    for (auto *arg : appliedTypeArgs) {
        names.push_back(arg ? arg->full_name : string("<null>"));
    }
    return names;
}

GenericInstanceKey
buildStructInstanceKey(const CompilationUnit &requesterUnit,
                       const CompilationUnit &ownerUnit,
                       const ModuleInterface::TypeDecl &typeDecl,
                       const std::vector<TypeClass *> &appliedTypeArgs) {
    GenericInstanceKey key;
    key.requesterModuleKey = requesterUnit.moduleKey();
    key.ownerModuleKey = ownerUnit.moduleKey();
    key.kind = GenericInstanceKind::Struct;
    key.templateName = typeDecl.exportedName;
    key.concreteTypeArgs = buildConcreteTypeArgNames(appliedTypeArgs);
    return key;
}

AstStructDecl *
findLocalStructDecl(const CompilationUnit &unit, llvm::StringRef localName) {
    auto *root = unit.syntaxTree();
    auto *program = dynamic_cast<AstProgram *>(root);
    auto *body = dynamic_cast<AstStatList *>(program ? program->body : root);
    if (!body) {
        return nullptr;
    }
    for (auto *stmt : body->getBody()) {
        auto *structDecl = dynamic_cast<AstStructDecl *>(stmt);
        if (!structDecl) {
            continue;
        }
        if (llvm::StringRef(structDecl->name.tochara(), structDecl->name.size()) ==
            localName) {
            return structDecl;
        }
    }
    return nullptr;
}

TypeClass *
substituteAppliedStructTemplateType(
    TypeTable *typeTable, const CompilationUnit &requesterUnit,
    const CompilationUnit &contextUnit, TypeNode *node,
    const std::unordered_map<std::string, TypeClass *> &genericArgs,
    const location &loc, const std::string &context) {
    if (!typeTable || !node) {
        return nullptr;
    }

    if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
        return substituteAppliedStructTemplateType(
            typeTable, requesterUnit, contextUnit, param->type, genericArgs,
            loc, context);
    }
    if (dynamic_cast<AnyTypeNode *>(node)) {
        return typeTable->createAnyType();
    }
    if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
        auto rawName = baseTypeName(base);
        if (auto found = genericArgs.find(rawName); found != genericArgs.end()) {
            return found->second;
        }
        return resolveTypeNode(typeTable, requesterUnit, node, false);
    }
    if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
        auto *base = dynamic_cast<BaseTypeNode *>(applied->base);
        auto *typeDecl = resolveVisibleTypeDecl(contextUnit, base);
        if (!typeDecl) {
            return resolveTypeNode(typeTable, requesterUnit, node, false);
        }
        if (!typeDecl->isGeneric()) {
            error(applied->loc,
                  "type `" + describeTypeNode(applied, "<unknown type>") +
                      "` applies `[...]` arguments to a non-generic type",
                  "Remove the `[...]` arguments, or make the base type generic "
                  "before specializing it.");
        }
        if (applied->args.size() != typeDecl->typeParams.size()) {
            error(applied->loc,
                  "generic type argument count mismatch for `" +
                      toStdString(typeDecl->exportedName) + "`: expected " +
                      std::to_string(typeDecl->typeParams.size()) + ", got " +
                      std::to_string(applied->args.size()),
                  "Match the number of `[` `]` type arguments to the generic "
                  "type parameter list.");
        }
        std::vector<TypeClass *> argTypes;
        argTypes.reserve(applied->args.size());
        for (auto *arg : applied->args) {
            auto *argType = substituteAppliedStructTemplateType(
                typeTable, requesterUnit, contextUnit, arg, genericArgs,
                arg ? arg->loc : loc,
                context);
            if (!argType) {
                error(arg ? arg->loc : loc,
                      "unknown type argument for `" +
                          describeTypeNode(applied, "<unknown type>") + "`: " +
                          describeTypeNode(arg, "void"));
            }
            argTypes.push_back(argType);
        }
        if (auto *ownerUnit = findTemplateOwnerUnit(contextUnit, *typeDecl)) {
            return requesterUnit.materializeAppliedStructType(
                typeTable, *typeDecl, std::move(argTypes), *ownerUnit);
        }
        return typeTable->createOpaqueStructType(
            buildAppliedTypeName(toStdString(typeDecl->exportedName), argTypes),
            typeDecl->declKind, typeDecl->exportedName, std::move(argTypes));
    }
    if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
        auto *baseType = substituteAppliedStructTemplateType(
            typeTable, requesterUnit, contextUnit, qualified->base,
            genericArgs, loc, context);
        return baseType ? typeTable->createConstType(baseType) : nullptr;
    }
    if (auto *dynType = dynamic_cast<DynTypeNode *>(node)) {
        return resolveTypeNode(typeTable, requesterUnit, dynType, false);
    }
    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        auto *baseType = substituteAppliedStructTemplateType(
            typeTable, requesterUnit, contextUnit, pointer->base, genericArgs,
            loc, context);
        for (uint32_t i = 0; baseType && i < pointer->dim; ++i) {
            baseType = typeTable->createPointerType(baseType);
        }
        return baseType;
    }
    if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
        auto *baseType = substituteAppliedStructTemplateType(
            typeTable, requesterUnit, contextUnit, indexable->base,
            genericArgs, loc, context);
        return baseType ? typeTable->createIndexablePointerType(baseType)
                        : nullptr;
    }
    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        auto *baseType = substituteAppliedStructTemplateType(
            typeTable, requesterUnit, contextUnit, array->base, genericArgs,
            loc, context);
        return baseType ? typeTable->createArrayType(baseType, array->dim)
                        : nullptr;
    }
    if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
        std::vector<TypeClass *> itemTypes;
        itemTypes.reserve(tuple->items.size());
        for (auto *item : tuple->items) {
            itemTypes.push_back(substituteAppliedStructTemplateType(
                typeTable, requesterUnit, contextUnit, item, genericArgs,
                item ? item->loc : loc, context));
        }
        return typeTable->getOrCreateTupleType(itemTypes);
    }
    if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
        std::vector<TypeClass *> argTypes;
        std::vector<BindingKind> argBindingKinds;
        argTypes.reserve(func->args.size());
        argBindingKinds.reserve(func->args.size());
        for (auto *arg : func->args) {
            argBindingKinds.push_back(funcParamBindingKind(arg));
            argTypes.push_back(substituteAppliedStructTemplateType(
                typeTable, requesterUnit, contextUnit,
                unwrapFuncParamType(arg), genericArgs,
                arg ? arg->loc : loc, context));
        }
        auto *retType = substituteAppliedStructTemplateType(
            typeTable, requesterUnit, contextUnit, func->ret, genericArgs,
            loc, context);
        auto *funcType = typeTable->getOrCreateFunctionType(
            argTypes, retType, std::move(argBindingKinds));
        return funcType ? typeTable->createPointerType(funcType) : nullptr;
    }

    return resolveTypeNode(typeTable, requesterUnit, node, false);
}

TypeClass *
resolveTypeNode(TypeTable *typeTable, const CompilationUnit &unit,
                TypeNode *node, bool validateLayout = true) {
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

    if (dynamic_cast<AnyTypeNode *>(node)) {
        resolved = typeTable->createAnyType();
        unit.cacheResolvedType(node, resolved);
        return resolved;
    }

    if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
        auto appliedName = describeTypeNode(applied, "<unknown type>");
        auto *base = dynamic_cast<BaseTypeNode *>(applied->base);
        const auto *typeDecl = resolveVisibleTypeDecl(unit, base);
        if (!typeDecl) {
            unit.cacheResolvedType(node, nullptr);
            return nullptr;
        }
        if (!typeDecl->isGeneric()) {
            error(applied->loc,
                  "type `" + appliedName +
                      "` applies `[...]` arguments to a non-generic type",
                  "Remove the `[...]` arguments, or make the base type generic "
                  "before specializing it.");
        }
        if (applied->args.size() != typeDecl->typeParams.size()) {
            error(applied->loc,
                  "generic type argument count mismatch for `" +
                      toStdString(typeDecl->exportedName) + "`: expected " +
                      std::to_string(typeDecl->typeParams.size()) + ", got " +
                      std::to_string(applied->args.size()),
                  "Match the number of `[` `]` type arguments to the generic "
                  "type parameter list.");
        }
        std::vector<TypeClass *> argTypes;
        argTypes.reserve(applied->args.size());
        for (auto *arg : applied->args) {
            auto *argType = resolveTypeNode(typeTable, unit, arg, false);
            if (!argType) {
                error(arg ? arg->loc : applied->loc,
                      "unknown type argument for `" + appliedName + "`: " +
                          describeTypeNode(arg, "void"));
            }
            argTypes.push_back(argType);
        }
        if (auto *ownerUnit =
                compilation_unit_impl::findTemplateOwnerUnit(unit, *typeDecl)) {
            resolved = unit.materializeAppliedStructType(
                typeTable, *typeDecl, std::move(argTypes), *ownerUnit);
        } else {
            resolved = typeTable->createOpaqueStructType(
                appliedName, typeDecl->declKind, typeDecl->exportedName,
                std::move(argTypes));
        }
        unit.cacheResolvedType(node, resolved);
        return resolved;
    }

    if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
        auto *baseType =
            resolveTypeNode(typeTable, unit, qualified->base, false);
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
                if (lookup.typeDecl && lookup.typeDecl->isGeneric()) {
                    errorBareGenericTemplateType(base->loc, rawName);
                }
                auto *type = typeTable->getType(lookup.resolvedName);
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
            if (lookup.typeDecl && lookup.typeDecl->isGeneric()) {
                errorBareGenericTemplateType(base->loc,
                                             moduleName + "." + memberName);
            }
        }
        resolved = typeTable->getType(llvm::StringRef(rawName));
        unit.cacheResolvedType(node, resolved);
        return resolved;
    }

    if (auto *dynType = dynamic_cast<DynTypeNode *>(node)) {
        bool readOnlyDataPtr = false;
        auto *base = getDynTraitBaseNode(dynType, &readOnlyDataPtr);
        if (!base) {
            unit.cacheResolvedType(node, nullptr);
            return nullptr;
        }

        auto rawName = baseTypeName(base);
        std::string moduleName;
        std::string memberName;
        CompilationUnit::TopLevelLookup lookup;
        if (!splitBaseTypeName(base, moduleName, memberName)) {
            lookup = unit.lookupTopLevelName(rawName);
        } else {
            const auto *imported = unit.findImportedModule(moduleName);
            if (!imported) {
                unit.cacheResolvedType(node, nullptr);
                return nullptr;
            }
            lookup = unit.lookupTopLevelName(*imported, memberName);
        }

        if (!lookup.isTrait()) {
            unit.cacheResolvedType(node, nullptr);
            return nullptr;
        }
        resolved =
            typeTable->createDynTraitType(lookup.resolvedName, readOnlyDataPtr);
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
        auto *elementType =
            resolveTypeNode(typeTable, unit, indexable->base, false);
        resolved = elementType
                       ? typeTable->createIndexablePointerType(elementType)
                       : nullptr;
        unit.cacheResolvedType(node, resolved);
        return resolved;
    }

    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        auto *elementType =
            resolveTypeNode(typeTable, unit, array->base, false);
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
            auto *argType = resolveTypeNode(typeTable, unit,
                                            unwrapFuncParamType(arg), false);
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

}  // namespace compilation_unit_impl

CompilationUnit::CompilationUnit(const SourceBuffer &source) {
    refreshSource(source);
}

const SourceBuffer &
CompilationUnit::source() const {
    assert(source_ != nullptr);
    return *source_;
}

AstNode *
CompilationUnit::requireSyntaxTree() const {
    if (syntaxTree_ == nullptr) {
        internalError("compilation unit `" + toStdString(path_) +
                          "` is missing its parsed syntax tree",
                      "Parse the unit before lowering or emission.");
    }
    return syntaxTree_;
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
CompilationUnit::attachInterface(
    std::shared_ptr<ModuleInterface> moduleInterface) {
    moduleInterface_ = std::move(moduleInterface);
    if (moduleInterface_) {
        moduleInterface_->refresh(path_, moduleKey_, moduleName_,
                                  hashModuleSource(source().content()));
    }
}

void
CompilationUnit::refreshSource(const SourceBuffer &source) {
    const auto newPath = source.path();
    const auto newKey = compilation_unit_impl::deriveModuleKey(newPath);
    const auto newName = compilation_unit_impl::deriveModuleName(newPath);
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
    stage_ =
        tree ? CompilationUnitStage::Parsed : CompilationUnitStage::Discovered;
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
    localTraitBindings_.clear();
    localFunctionBindings_.clear();
    localGlobalBindings_.clear();
}

bool
CompilationUnit::addImportedModule(string alias, const CompilationUnit &unit) {
    ImportedModule imported = {unit.path(), unit.moduleKey(), unit.moduleName(),
                               unit.interface(), &unit};
    return importedModules_.emplace(std::move(alias), std::move(imported))
        .second;
}

const CompilationUnit::ImportedModule *
CompilationUnit::findImportedModule(const ::string &alias) const {
    auto found = importedModules_.find(alias);
    if (found == importedModules_.end()) {
        return nullptr;
    }
    return &found->second;
}

const CompilationUnit::ImportedModule *
CompilationUnit::findImportedModuleByInterface(
    const ModuleInterface *interface) const {
    if (!interface) {
        return nullptr;
    }
    for (const auto &entry : importedModules_) {
        if (entry.second.interface == interface) {
            return &entry.second;
        }
    }
    return nullptr;
}

bool
CompilationUnit::importsModule(const ::string &alias) const {
    return findImportedModule(alias) != nullptr;
}

CompilationUnit::TopLevelLookup
CompilationUnit::lookupTopLevelName(const ::string &name) const {
    TopLevelLookup lookup;

    if (const auto *typeName = findLocalType(name)) {
        lookup.kind = TopLevelLookupKind::Type;
        lookup.resolvedName = *typeName;
        if (moduleInterface_) {
            lookup.typeDecl = moduleInterface_->findType(name);
        }
        return lookup;
    }

    if (const auto *traitName = findLocalTrait(name)) {
        lookup.kind = TopLevelLookupKind::Trait;
        lookup.resolvedName = *traitName;
        if (moduleInterface_) {
            lookup.traitDecl = moduleInterface_->findTrait(name);
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

    if (const auto *globalName = findLocalGlobal(name)) {
        lookup.kind = TopLevelLookupKind::Global;
        lookup.resolvedName = *globalName;
        if (moduleInterface_) {
            lookup.globalDecl = moduleInterface_->findGlobal(name);
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
                                    const ::string &name) const {
    TopLevelLookup lookup;
    if (!moduleNamespace.interface) {
        return lookup;
    }

    auto member = moduleNamespace.interface->lookupTopLevelName(name);
    if (member.isType()) {
        lookup.kind = TopLevelLookupKind::Type;
        lookup.importedModule = &moduleNamespace;
        lookup.typeDecl = member.typeDecl;
        lookup.resolvedName =
            member.typeDecl ? member.typeDecl->exportedName : string();
        return lookup;
    }
    if (member.isTrait()) {
        lookup.kind = TopLevelLookupKind::Trait;
        lookup.importedModule = &moduleNamespace;
        lookup.traitDecl = member.traitDecl;
        lookup.resolvedName =
            member.traitDecl ? member.traitDecl->exportedName : string();
        return lookup;
    }
    if (member.isFunction()) {
        lookup.kind = TopLevelLookupKind::Function;
        lookup.importedModule = &moduleNamespace;
        lookup.functionDecl = member.functionDecl;
        lookup.resolvedName =
            member.functionDecl ? member.functionDecl->symbolName : string();
        return lookup;
    }
    if (member.isGlobal()) {
        lookup.kind = TopLevelLookupKind::Global;
        lookup.importedModule = &moduleNamespace;
        lookup.globalDecl = member.globalDecl;
        lookup.resolvedName =
            member.globalDecl ? member.globalDecl->symbolName : string();
        return lookup;
    }
    return lookup;
}

const ModuleInterface::TraitDecl *
CompilationUnit::findVisibleTraitByResolvedName(
    const ::string &resolvedName) const {
    if (moduleInterface_) {
        if (const auto *traitDecl =
                moduleInterface_->findTraitByExportedName(resolvedName)) {
            return traitDecl;
        }
    }

    for (const auto &entry : importedModules_) {
        const auto &imported = entry.second;
        if (!imported.interface) {
            continue;
        }
        if (const auto *traitDecl =
                imported.interface->findTraitByExportedName(resolvedName)) {
            return traitDecl;
        }
    }

    return nullptr;
}

std::vector<CompilationUnit::VisibleTraitImpl>
CompilationUnit::findVisibleTraitImpls(const ::string &traitName,
                                       const ::string &selfTypeSpelling) const {
    std::vector<VisibleTraitImpl> matches;

    if (moduleInterface_) {
        for (const auto &implDecl : moduleInterface_->traitImpls()) {
            if (implDecl.traitName == traitName &&
                implDecl.selfTypeSpelling == selfTypeSpelling) {
                matches.push_back(VisibleTraitImpl{&implDecl, nullptr});
            }
        }
    }

    for (const auto &entry : importedModules_) {
        const auto &imported = entry.second;
        if (!imported.interface) {
            continue;
        }
        for (const auto &implDecl : imported.interface->traitImpls()) {
            if (implDecl.traitName == traitName &&
                implDecl.selfTypeSpelling == selfTypeSpelling) {
                matches.push_back(VisibleTraitImpl{&implDecl, &imported});
            }
        }
    }

    return matches;
}

std::vector<CompilationUnit::VisibleTraitImpl>
CompilationUnit::findVisibleTraitImpls(const ::string &traitName,
                                       TypeClass *selfType) const {
    std::vector<VisibleTraitImpl> matches;
    std::unordered_set<std::string> active;

    if (moduleInterface_) {
        for (const auto &implDecl : moduleInterface_->traitImpls()) {
            if (implDecl.traitName != traitName) {
                continue;
            }
            if (compilation_unit_impl::traitImplMatchesConcreteSelfType(
                    *this, *this, implDecl, selfType, active)) {
                matches.push_back(VisibleTraitImpl{&implDecl, nullptr});
            }
        }
    }

    for (const auto &entry : importedModules_) {
        const auto &imported = entry.second;
        if (!imported.interface || !imported.unit) {
            continue;
        }
        for (const auto &implDecl : imported.interface->traitImpls()) {
            if (implDecl.traitName != traitName) {
                continue;
            }
            if (compilation_unit_impl::traitImplMatchesConcreteSelfType(
                    *this, *imported.unit, implDecl, selfType, active)) {
                matches.push_back(VisibleTraitImpl{&implDecl, &imported});
            }
        }
    }

    return matches;
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
    materializingAppliedStructs_.clear();
    recordedGenericInstances_.clear();
}

void
CompilationUnit::ensureHashes() const {
    if (hashesReady_) {
        return;
    }
    interfaceHash_ = syntaxTree_
                         ? compilation_unit_impl::combineHash(
                               compilation_unit_impl::computeInterfaceHash(
                                   syntaxTree_),
                               compilation_unit_impl::
                                   computeVisibleImportInterfaceHash(*this))
                         : 0;
    implementationHash_ = sourceHash();
    hashesReady_ = true;
}

bool
CompilationUnit::bindLocalType(string localName, string resolvedName) {
    return localTypeBindings_
        .emplace(std::move(localName), std::move(resolvedName))
        .second;
}

bool
CompilationUnit::bindLocalTrait(string localName, string resolvedName) {
    return localTraitBindings_
        .emplace(std::move(localName), std::move(resolvedName))
        .second;
}

bool
CompilationUnit::bindLocalFunction(string localName, string resolvedName) {
    return localFunctionBindings_
        .emplace(std::move(localName), std::move(resolvedName))
        .second;
}

bool
CompilationUnit::bindLocalGlobal(string localName, string resolvedName) {
    return localGlobalBindings_
        .emplace(std::move(localName), std::move(resolvedName))
        .second;
}

const string *
CompilationUnit::findLocalType(const ::string &localName) const {
    auto found = localTypeBindings_.find(localName);
    if (found == localTypeBindings_.end()) {
        return nullptr;
    }
    return &found->second;
}

const string *
CompilationUnit::findLocalTrait(const ::string &localName) const {
    auto found = localTraitBindings_.find(localName);
    if (found == localTraitBindings_.end()) {
        return nullptr;
    }
    return &found->second;
}

const string *
CompilationUnit::findLocalFunction(const ::string &localName) const {
    auto found = localFunctionBindings_.find(localName);
    if (found == localFunctionBindings_.end()) {
        return nullptr;
    }
    return &found->second;
}

const string *
CompilationUnit::findLocalGlobal(const ::string &localName) const {
    auto found = localGlobalBindings_.find(localName);
    if (found == localGlobalBindings_.end()) {
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
    materializingAppliedStructs_.clear();
    recordedGenericInstances_.clear();
}

void
CompilationUnit::recordGenericInstance(GenericInstanceArtifactRecord record) const {
    for (auto &existing : recordedGenericInstances_) {
        if (!(existing.key == record.key)) {
            continue;
        }
        existing.revision = record.revision;
        for (const auto &symbol : record.emittedSymbolNames) {
            auto found = std::find(existing.emittedSymbolNames.begin(),
                                   existing.emittedSymbolNames.end(), symbol);
            if (found == existing.emittedSymbolNames.end()) {
                existing.emittedSymbolNames.push_back(symbol);
            }
        }
        return;
    }
    recordedGenericInstances_.push_back(std::move(record));
}

TypeClass *
CompilationUnit::resolveType(TypeTable *typeTable, TypeNode *node) const {
    return compilation_unit_impl::resolveTypeNode(typeTable, *this, node);
}

bool
CompilationUnit::ownsTypeDecl(const ModuleInterface::TypeDecl *typeDecl) const {
    if (!typeDecl || !moduleInterface_) {
        return false;
    }
    return moduleInterface_->findType(toStdString(typeDecl->localName)) ==
           typeDecl;
}

const CompilationUnit *
CompilationUnit::ownerUnitForTypeDecl(
    const ModuleInterface::TypeDecl *typeDecl) const {
    if (!typeDecl) {
        return nullptr;
    }
    if (ownsTypeDecl(typeDecl)) {
        return this;
    }
    for (const auto &entry : importedModules_) {
        const auto &imported = entry.second;
        if (!imported.unit || !imported.interface) {
            continue;
        }
        if (imported.interface->findType(toStdString(typeDecl->localName)) ==
            typeDecl) {
            return imported.unit;
        }
    }
    return nullptr;
}

const CompilationUnit *
CompilationUnit::contextUnitForInterface(
    const ModuleInterface *ownerInterface) const {
    if (!ownerInterface || ownerInterface == interface()) {
        return this;
    }
    if (const auto *imported = findImportedModuleByInterface(ownerInterface)) {
        return imported->unit;
    }
    return nullptr;
}

std::uint64_t
CompilationUnit::visibleImportInterfaceHash() const {
    return compilation_unit_impl::computeVisibleImportInterfaceHash(*this);
}

std::uint64_t
CompilationUnit::visibleTraitImplHash() const {
    return compilation_unit_impl::computeVisibleTraitImplHash(*this);
}

StructType *
CompilationUnit::materializeAppliedStructType(
    TypeTable *typeTable, const ModuleInterface::TypeDecl &typeDecl,
    std::vector<TypeClass *> appliedTypeArgs,
    const CompilationUnit &contextUnit) const {
    if (!typeTable) {
        return nullptr;
    }

    const auto *templateOwnerUnit =
        compilation_unit_impl::findTemplateOwnerUnit(contextUnit, typeDecl);
    if (!templateOwnerUnit) {
        return nullptr;
    }

    const auto baseAppliedName =
        templateOwnerUnit == this ? toStdString(typeDecl.localName)
                                  : toStdString(typeDecl.exportedName);
    auto appliedName = compilation_unit_impl::buildAppliedTypeName(
        baseAppliedName, appliedTypeArgs);
    auto *structType = typeTable->createOpaqueStructType(
        string(appliedName), typeDecl.declKind, typeDecl.exportedName,
        appliedTypeArgs, templateOwnerUnit);
    if (!structType) {
        return nullptr;
    }

    auto instanceKey = compilation_unit_impl::buildStructInstanceKey(
        *this, *templateOwnerUnit, typeDecl, appliedTypeArgs);
    auto revision = GenericTemplateRevision{
        templateOwnerUnit->interfaceHash(), templateOwnerUnit->implementationHash(),
        templateOwnerUnit->visibleImportInterfaceHash(),
        visibleTraitImplHash()};
    recordGenericInstance({instanceKey, revision, {}});

    auto [_, inserted] = materializingAppliedStructs_.insert(instanceKey);
    if (!inserted) {
        return structType;
    }
    struct Guard {
        std::unordered_set<GenericInstanceKey, GenericInstanceKeyHash> &active;
        GenericInstanceKey key;
        ~Guard() { active.erase(key); }
    } guard{materializingAppliedStructs_, std::move(instanceKey)};

    std::unordered_map<std::string, TypeClass *> genericArgs;
    if (typeDecl.typeParams.size() != appliedTypeArgs.size()) {
        throw DiagnosticError(
            DiagnosticError::Category::Internal,
            "generic struct `" + toStdString(typeDecl.localName) +
                "` instance is missing concrete type arguments",
            "This looks like an applied-struct instantiation bug.");
    }
    genericArgs.reserve(typeDecl.typeParams.size());
    for (std::size_t i = 0; i < typeDecl.typeParams.size(); ++i) {
        genericArgs.emplace(toStdString(typeDecl.typeParams[i].localName),
                            appliedTypeArgs[i]);
    }

    if (structType->isOpaque()) {
        auto *structDecl = compilation_unit_impl::findLocalStructDecl(
            *templateOwnerUnit, toStringRef(typeDecl.localName));
        if (!structDecl) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "generic struct `" +
                    toStdString(typeDecl.localName) +
                    "` is missing its template AST",
                "This looks like a generic template registration bug.");
        }

        auto *body = dynamic_cast<AstStatList *>(structDecl->body);
        llvm::StringMap<StructType::ValueTy> members;
        llvm::StringMap<AccessKind> memberAccess;
        llvm::StringSet<> embeddedMembers;
        std::unordered_map<std::string, location> seenMembers;
        int nextMemberIndex = 0;

        if (body) {
            for (auto *stmt : body->getBody()) {
                auto *fieldDecl = dynamic_cast<AstVarDecl *>(stmt);
                if (!fieldDecl) {
                    continue;
                }
                auto *fieldType =
                    compilation_unit_impl::substituteAppliedStructTemplateType(
                        typeTable, *this, *templateOwnerUnit,
                        fieldDecl->typeNode, genericArgs,
                        fieldDecl->loc,
                        "struct field `" +
                            declarationsupport_impl::describeStructFieldSyntax(
                                fieldDecl) +
                            "`");
                if (!fieldType) {
                    error(fieldDecl->loc,
                          "unknown struct field type for `" +
                              declarationsupport_impl::describeStructFieldSyntax(
                                  fieldDecl) +
                              "`: " +
                              describeTypeNode(fieldDecl->typeNode, "void"));
                }
                declarationsupport_impl::rejectBareFunctionType(
                    fieldType, fieldDecl->typeNode,
                    "unsupported bare function struct field type for `" +
                        declarationsupport_impl::describeStructFieldSyntax(
                            fieldDecl) +
                        "`",
                    fieldDecl->loc);
                declarationsupport_impl::validateStructFieldType(
                    structDecl, fieldDecl, fieldType);
                declarationsupport_impl::validateEmbeddedStructField(
                    structDecl, fieldDecl, fieldType);
                declarationsupport_impl::insertStructMember(
                    structDecl, fieldDecl, fieldType, members, memberAccess,
                    embeddedMembers, seenMembers, nextMemberIndex);
            }
        }

        structType->complete(members, memberAccess, embeddedMembers);
    }

    for (const auto &method : typeDecl.methodTemplates) {
        if (structType->getMethodType(toStringRef(method.localName))) {
            continue;
        }

        std::vector<TypeClass *> argTypes;
        argTypes.reserve(method.paramTypeNodes.size() + 1);
        auto *selfPointee = declarationsupport_impl::methodReceiverPointeeType(
            typeTable, structType, method.receiverAccess);
        argTypes.push_back(typeTable->createPointerType(selfPointee));
        for (std::size_t i = 0; i < method.paramTypeNodes.size(); ++i) {
            auto *paramType =
                compilation_unit_impl::substituteAppliedStructTemplateType(
                    typeTable, *this, *templateOwnerUnit,
                    method.paramTypeNodes[i], genericArgs,
                    method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                             : location(),
                    "parameter `" +
                        (i < method.paramNames.size()
                             ? toStdString(method.paramNames[i])
                             : std::string("<param>")) +
                        "` in method `" + toStdString(typeDecl.localName) +
                        "." + toStdString(method.localName) + "`");
            if (!paramType) {
                error(method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                               : location(),
                      "unknown method parameter type in `" +
                          toStdString(typeDecl.localName) + "." +
                          toStdString(method.localName) + "`");
            }
            declarationsupport_impl::rejectBareFunctionType(
                paramType, method.paramTypeNodes[i],
                "unsupported bare function parameter type in `" +
                    toStdString(typeDecl.localName) + "." +
                    toStdString(method.localName) + "`",
                method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                         : location());
            declarationsupport_impl::rejectOpaqueStructByValue(
                paramType, method.paramTypeNodes[i],
                method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                         : location(),
                "parameter `" +
                    (i < method.paramNames.size()
                         ? toStdString(method.paramNames[i])
                         : std::string("<param>")) +
                    "` in method `" + toStdString(typeDecl.localName) + "." +
                    toStdString(method.localName) + "`");
            argTypes.push_back(paramType);
        }

        TypeClass *retType = nullptr;
        if (method.returnTypeNode) {
            retType = compilation_unit_impl::substituteAppliedStructTemplateType(
                typeTable, *this, *templateOwnerUnit, method.returnTypeNode,
                genericArgs,
                method.returnTypeNode->loc,
                "return type of method `" + toStdString(typeDecl.localName) +
                    "." + toStdString(method.localName) + "`");
            declarationsupport_impl::rejectBareFunctionType(
                retType, method.returnTypeNode,
                "unsupported bare function return type for method `" +
                    toStdString(typeDecl.localName) + "." +
                    toStdString(method.localName) + "`",
                method.returnTypeNode->loc);
            declarationsupport_impl::rejectOpaqueStructByValue(
                retType, method.returnTypeNode, method.returnTypeNode->loc,
                "return type of method `" + toStdString(typeDecl.localName) +
                    "." + toStdString(method.localName) + "`");
        }

        auto *funcType = typeTable->getOrCreateFunctionType(
            argTypes, retType, method.paramBindingKinds, AbiKind::Native);
        structType->addMethodType(toStringRef(method.localName), funcType,
                                  method.paramNames);
    }

    return structType;
}

StructType *
CompilationUnit::materializeLocalAppliedStructType(
    TypeTable *typeTable, const ModuleInterface::TypeDecl &typeDecl,
    std::vector<TypeClass *> appliedTypeArgs) const {
    if (!ownsTypeDecl(&typeDecl)) {
        return nullptr;
    }
    return materializeAppliedStructType(typeTable, typeDecl,
                                        std::move(appliedTypeArgs), *this);
}

}  // namespace lona
