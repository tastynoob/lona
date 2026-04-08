#include "lona/abi/abi.hh"
#include "lona/ast/astnode.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/declare/support.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/module/module_interface.hh"
#include "lona/sema/moduleentry.hh"
#include "lona/sema/initializer.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include <cassert>
#include <cstdint>
#include <llvm-18/llvm/IR/Function.h>
#include <llvm-18/llvm/IR/GlobalVariable.h>
#include <llvm-18/llvm/IR/Module.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lona {

using declarationsupport_impl::declareModuleNamespace;
using declarationsupport_impl::describeStructFieldSyntax;
using declarationsupport_impl::extractParamBindingKinds;
using declarationsupport_impl::extractParamNames;
using declarationsupport_impl::insertStructMember;
using declarationsupport_impl::interfaceMethodReceiverPointeeType;
using declarationsupport_impl::recordTopLevelDeclName;
using declarationsupport_impl::rejectBareFunctionType;
using declarationsupport_impl::rejectOpaqueStructByValue;
using declarationsupport_impl::requireTypeTable;
using declarationsupport_impl::TopLevelDeclKind;
using declarationsupport_impl::validateEmbeddedStructField;
using declarationsupport_impl::validateExternCFunctionSignature;
using declarationsupport_impl::validateFunctionReceiverAccess;
using declarationsupport_impl::validateStructDeclShape;
using declarationsupport_impl::validateStructFieldType;

namespace moduleinterface_impl {

[[noreturn]] void
reportGlobalConflict(const CompilationUnit *unit, llvm::StringRef globalName,
                     TypeClass *existingType, TypeClass *incomingType) {
    std::string message =
        "conflicting declarations for global `" + globalName.str() + "`";
    if (existingType && incomingType) {
        message += ": `" + describeResolvedType(existingType) + "` vs `" +
                   describeResolvedType(incomingType) + "`";
    }

    std::string hint =
        "Make duplicated `#[extern] global` declarations use the same type in "
        "every module.";
    if (unit && unit->syntaxTree()) {
        throw DiagnosticError(DiagnosticError::Category::Semantic,
                              unit->syntaxTree()->loc, std::move(message),
                              std::move(hint));
    }
    throw DiagnosticError(DiagnosticError::Category::Semantic,
                          std::move(message), std::move(hint));
}

[[noreturn]] void
reportFunctionConflict(const CompilationUnit *unit, llvm::StringRef functionName,
                       FuncType *existingType, FuncType *incomingType) {
    std::string message =
        "conflicting declarations for function `" + functionName.str() + "`";
    if (existingType && incomingType) {
        message += ": `" + describeResolvedType(existingType) + "` vs `" +
                   describeResolvedType(incomingType) + "`";
    }

    std::string hint =
        "Make duplicated exported or `#[extern \"C\"]` function declarations "
        "use the same signature in every module.";
    if (unit && unit->syntaxTree()) {
        throw DiagnosticError(DiagnosticError::Category::Semantic,
                              unit->syntaxTree()->loc, std::move(message),
                              std::move(hint));
    }
    throw DiagnosticError(DiagnosticError::Category::Semantic,
                          std::move(message), std::move(hint));
}

TypeClass *
lookupBuiltinType(llvm::StringRef name) {
    if (name == "u8") return u8Ty;
    if (name == "i8") return i8Ty;
    if (name == "u16") return u16Ty;
    if (name == "i16") return i16Ty;
    if (name == "u32") return u32Ty;
    if (name == "i32") return i32Ty;
    if (name == "u64") return u64Ty;
    if (name == "i64") return i64Ty;
    if (name == "usize") return usizeTy;
    if (name == "int") return i32Ty;
    if (name == "uint") return u32Ty;
    if (name == "f32") return f32Ty;
    if (name == "f64") return f64Ty;
    if (name == "bool") return boolTy;
    return nullptr;
}

class InterfaceCollector {
    CompilationUnit &unit_;
    ModuleInterface *interface_;
    std::vector<AstStructDecl *> structDecls_;
    std::vector<AstTraitDecl *> traitDecls_;
    std::vector<AstTraitImplDecl *> traitImplDecls_;
    std::vector<AstFuncDecl *> funcDecls_;
    std::vector<AstGlobalDecl *> globalDecls_;
    std::unordered_set<std::string> materializingAppliedStructs_;
    enum class StructCompletionState {
        Pending,
        Completing,
        Completed,
    };
    std::unordered_map<const AstStructDecl *, StructCompletionState>
        structCompletionStates_;
    std::unordered_map<const StructType *, AstStructDecl *> localStructDecls_;
    std::unordered_map<std::string, std::pair<TopLevelDeclKind, location>>
        topLevelDecls_;

    struct ResolvedTraitRef {
        const ModuleInterface::TraitDecl *decl = nullptr;
        string resolvedName;
        bool localToUnit = false;
    };

    struct ResolvedSelfTypeRef {
        StructType *structType = nullptr;
        const ModuleInterface::TypeDecl *typeDecl = nullptr;
        string resolvedName;
        bool localToUnit = false;
        bool concreteMethodValidation = false;
    };

    struct ValidatedTraitImpl {
        ModuleInterface::TraitImplDecl decl;
        location loc;
    };

    struct CollectedFunctionInterface {
        FuncType *type = nullptr;
        std::vector<string> paramNames;
        std::vector<BindingKind> paramBindingKinds;
        std::vector<TypeNode *> paramTypeNodes;
        std::vector<string> paramTypeSpellings;
        TypeNode *returnTypeNode = nullptr;
        string returnTypeSpelling = "void";
        std::vector<ModuleInterface::GenericParamDecl> typeParams;

        bool isGeneric() const { return !typeParams.empty(); }
    };

    enum class GenericParamContext {
        StructDecl,
        FunctionDecl,
        TraitImplHeader,
    };

    std::vector<ModuleInterface::GenericParamDecl>
    collectGenericParams(const std::vector<AstGenericParam *> *params,
                         GenericParamContext context) {
        std::vector<ModuleInterface::GenericParamDecl> collected;
        if (!params) {
            return collected;
        }
        collected.reserve(params->size());
        for (auto *param : *params) {
            if (!param) {
                continue;
            }
            ModuleInterface::GenericParamDecl decl{param->name.text, string()};
            if (param->hasBoundTrait()) {
                auto traitRef = resolveTraitRef(param->boundTrait,
                                                param->boundTrait->loc);
                decl.boundTraitName = traitRef.resolvedName;
            }
            collected.push_back(std::move(decl));
        }
        return collected;
    }

    static std::unordered_set<std::string>
    collectGenericParamNames(
        const std::vector<ModuleInterface::GenericParamDecl> &params) {
        std::unordered_set<std::string> names;
        names.reserve(params.size());
        for (const auto &param : params) {
            names.insert(toStdString(param.localName));
        }
        return names;
    }

    static std::string genericTemplateHint(const std::string &rawName) {
        return "Write `" + rawName +
               "[...]` with explicit type arguments, or use the template "
               "name only inside another applied type like `" + rawName +
               "[T]`.";
    }

    static BaseTypeNode *rootSelfTypeBase(TypeNode *node) {
        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            return base;
        }
        if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
            return rootSelfTypeBase(applied->base);
        }
        return nullptr;
    }

    static string qualifySelfTypeSpelling(TypeNode *node,
                                          const ModuleInterface::TypeDecl *decl) {
        if (!node) {
            return {};
        }
        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            return decl ? decl->exportedName
                        : string(describeTypeNode(base, "<unknown type>"));
        }
        if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
            string text = qualifySelfTypeSpelling(applied->base, decl) + "[";
            for (std::size_t i = 0; i < applied->args.size(); ++i) {
                if (i != 0) {
                    text += ", ";
                }
                text += describeTypeNode(applied->args[i], "void");
            }
            text += "]";
            return text;
        }
        return string(describeTypeNode(node, "<unknown type>"));
    }

    [[noreturn]] void errorBareGenericTemplateType(const location &loc,
                                                   const std::string &rawName) {
        error(loc,
              "generic type template `" + rawName +
                  "` requires explicit `[...]` type arguments",
              genericTemplateHint(rawName));
    }

    const ModuleInterface::TypeDecl *
    resolveVisibleTypeDecl(BaseTypeNode *base,
                           const CompilationUnit &lookupUnit,
                           const ModuleInterface *lookupInterface) const {
        if (!base || !lookupInterface) {
            return nullptr;
        }
        auto rawName = baseTypeName(base);
        std::string moduleName;
        std::string typeName;
        if (!splitBaseTypeName(base, moduleName, typeName)) {
            auto lookup = lookupInterface->lookupTopLevelName(rawName);
            return lookup.isType() ? lookup.typeDecl : nullptr;
        }

        const auto *imported = findImportedModuleForLookup(lookupUnit, moduleName);
        if (!imported || !imported->interface) {
            return nullptr;
        }
        auto lookup = lookupUnit.lookupTopLevelName(*imported, typeName);
        return lookup.isType() ? lookup.typeDecl : nullptr;
    }

    const ModuleInterface::TypeDecl *
    resolveVisibleTypeDecl(BaseTypeNode *base) {
        return resolveVisibleTypeDecl(base, unit_, interface_);
    }

    const ModuleInterface *
    interfaceForLookupUnit(const CompilationUnit &lookupUnit) const {
        if (&lookupUnit == &unit_) {
            return interface_;
        }
        return lookupUnit.interface();
    }

    const CompilationUnit::ImportedModule *findImportedModuleForLookup(
        const CompilationUnit &lookupUnit, llvm::StringRef moduleName) const {
        const auto *imported = lookupUnit.findImportedModule(moduleName.str());
        if (!imported) {
            return nullptr;
        }
        if (imported->unit && !imported->unit->interfaceCollected()) {
            ensureUnitInterfaceCollected(
                *const_cast<CompilationUnit *>(imported->unit));
        }
        return imported;
    }

    TypeClass *resolveType(TypeNode *node, const CompilationUnit &lookupUnit,
                           bool validateLayout = true) {
        auto *lookupInterface = interfaceForLookupUnit(lookupUnit);
        if (!node || !lookupInterface) {
            return nullptr;
        }
        if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
            return resolveType(param->type, lookupUnit, false);
        }
        if (validateLayout) {
            validateTypeNodeLayout(node);
        }
        if (dynamic_cast<AnyTypeNode *>(node)) {
            return interface_->getOrCreateAnyType();
        }
        if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
            return resolveAppliedType(applied, lookupUnit);
        }
        if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
            auto *baseType = resolveType(qualified->base, lookupUnit, false);
            return baseType ? interface_->getOrCreateConstType(baseType)
                            : nullptr;
        }
        if (auto *dynType = dynamic_cast<DynTypeNode *>(node)) {
            bool readOnlyDataPtr = false;
            auto *base = getDynTraitBaseNode(dynType, &readOnlyDataPtr);
            if (!base) {
                return nullptr;
            }

            auto rawName = baseTypeName(base);
            std::string moduleName;
            std::string traitName;
            if (!splitBaseTypeName(base, moduleName, traitName)) {
                auto lookup = lookupInterface->lookupTopLevelName(rawName);
                if (!lookup.isTrait() || !lookup.traitDecl) {
                    return nullptr;
                }
                return interface_->getOrCreateDynTraitType(
                    lookup.traitDecl->exportedName, readOnlyDataPtr);
            }

            const auto *imported =
                findImportedModuleForLookup(lookupUnit, moduleName);
            if (!imported || !imported->interface) {
                return nullptr;
            }
            auto lookup = lookupUnit.lookupTopLevelName(*imported, traitName);
            if (!lookup.isTrait() || !lookup.traitDecl) {
                return nullptr;
            }
            return interface_->getOrCreateDynTraitType(
                lookup.traitDecl->exportedName, readOnlyDataPtr);
        }
        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            auto rawName = baseTypeName(base);
            std::string moduleName;
            std::string typeName;
            if (!splitBaseTypeName(base, moduleName, typeName)) {
                if (isReservedInitialListTypeName(
                        llvm::StringRef(rawName.c_str(), rawName.size()))) {
                    errorReservedInitialListType(base->loc);
                }
                if (auto *builtin = lookupBuiltinType(
                        llvm::StringRef(rawName.c_str(), rawName.size()))) {
                    return builtin;
                }
                auto lookup = lookupInterface->lookupTopLevelName(rawName);
                if (lookup.isType() && lookup.typeDecl) {
                    if (lookup.typeDecl->isGeneric()) {
                        errorBareGenericTemplateType(base->loc, rawName);
                    }
                    return lookup.typeDecl->type;
                }
                return nullptr;
            }

            const auto *imported =
                findImportedModuleForLookup(lookupUnit, moduleName);
            if (!imported || !imported->interface) {
                return nullptr;
            }
            auto lookup = lookupUnit.lookupTopLevelName(*imported, typeName);
            if (lookup.isType() && lookup.typeDecl) {
                if (lookup.typeDecl->isGeneric()) {
                    errorBareGenericTemplateType(base->loc,
                                                 moduleName + "." + typeName);
                }
                return lookup.typeDecl->type;
            }
            return nullptr;
        }
        if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
            auto *baseType = resolveType(pointer->base, lookupUnit, false);
            for (uint32_t i = 0; baseType && i < pointer->dim; ++i) {
                baseType = interface_->getOrCreatePointerType(baseType);
            }
            return baseType;
        }
        if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
            auto *elementType = resolveType(indexable->base, lookupUnit, false);
            return elementType ? interface_->getOrCreateIndexablePointerType(
                                     elementType)
                               : nullptr;
        }
        if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
            auto *elementType = resolveType(array->base, lookupUnit, false);
            if (!elementType) {
                return nullptr;
            }
            return interface_->getOrCreateArrayType(elementType, array->dim);
        }
        if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
            std::vector<TypeClass *> itemTypes;
            itemTypes.reserve(tuple->items.size());
            for (auto *item : tuple->items) {
                auto *itemType = resolveType(item, lookupUnit, false);
                if (!itemType) {
                    return nullptr;
                }
                itemTypes.push_back(itemType);
            }
            return interface_->getOrCreateTupleType(itemTypes);
        }
        if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
            std::vector<TypeClass *> argTypes;
            std::vector<BindingKind> argBindingKinds;
            argTypes.reserve(func->args.size());
            argBindingKinds.reserve(func->args.size());
            for (auto *arg : func->args) {
                argBindingKinds.push_back(funcParamBindingKind(arg));
                auto *argType =
                    resolveType(unwrapFuncParamType(arg), lookupUnit, false);
                if (!argType) {
                    return nullptr;
                }
                argTypes.push_back(argType);
            }
            auto *retType = resolveType(func->ret, lookupUnit, false);
            auto *funcType = interface_->getOrCreateFunctionType(
                argTypes, retType, std::move(argBindingKinds));
            return funcType ? interface_->getOrCreatePointerType(funcType)
                            : nullptr;
        }
        return nullptr;
    }

    bool ownsTypeDecl(const ModuleInterface::TypeDecl *typeDecl) const {
        return typeDecl &&
               interface_->findType(toStdString(typeDecl->localName)) ==
                   typeDecl;
    }

    static const ModuleInterface::TypeDecl *
    findTypeDeclByTemplateName(const ModuleInterface *interface,
                               llvm::StringRef templateName) {
        if (!interface || templateName.empty()) {
            return nullptr;
        }
        for (const auto &entry : interface->types()) {
            if (toStringRef(entry.second.localName) == templateName ||
                toStringRef(entry.second.exportedName) == templateName) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    const ModuleInterface::TypeDecl *
    findVisibleTypeDeclByTemplateName(
        llvm::StringRef templateName,
        const CompilationUnit **ownerUnitOut = nullptr) const {
        if (ownerUnitOut) {
            *ownerUnitOut = nullptr;
        }
        if (templateName.empty()) {
            return nullptr;
        }
        if (auto *typeDecl = findTypeDeclByTemplateName(interface_, templateName)) {
            if (ownerUnitOut) {
                *ownerUnitOut = &unit_;
            }
            return typeDecl;
        }
        for (const auto &entry : unit_.importedModules()) {
            const auto &imported = entry.second;
            if (!imported.interface) {
                continue;
            }
            if (auto *typeDecl =
                    findTypeDeclByTemplateName(imported.interface, templateName)) {
                if (ownerUnitOut) {
                    *ownerUnitOut = imported.unit;
                }
                return typeDecl;
            }
        }
        return nullptr;
    }

    static AstStructDecl *findStructDeclInUnit(const CompilationUnit *unit,
                                               llvm::StringRef localName) {
        if (!unit) {
            return nullptr;
        }
        auto *root = unit->syntaxTree();
        auto *program = dynamic_cast<AstProgram *>(root);
        auto *body =
            dynamic_cast<AstStatList *>(program ? program->body : root);
        if (!body) {
            return nullptr;
        }
        for (auto *stmt : body->getBody()) {
            auto *structDecl = dynamic_cast<AstStructDecl *>(stmt);
            if (!structDecl) {
                continue;
            }
            if (toStringRef(structDecl->name) == localName) {
                return structDecl;
            }
        }
        return nullptr;
    }

    static std::string buildAppliedTypeName(const std::string &baseName,
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

    TypeClass *substituteAppliedStructTemplateType(
        TypeNode *node,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const location &loc, const std::string &context,
        const CompilationUnit &lookupUnit) {
        if (!node) {
            return nullptr;
        }
        if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
            return substituteAppliedStructTemplateType(param->type, genericArgs,
                                                       loc, context, lookupUnit);
        }
        if (dynamic_cast<AnyTypeNode *>(node)) {
            return interface_->getOrCreateAnyType();
        }
        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            auto rawName = baseTypeName(base);
            if (auto found = genericArgs.find(rawName); found != genericArgs.end()) {
                return found->second;
            }
            return resolveType(node, lookupUnit, false);
        }
        if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
            auto *base = dynamic_cast<BaseTypeNode *>(applied->base);
            const auto *typeDecl =
                resolveVisibleTypeDecl(base, lookupUnit,
                                       interfaceForLookupUnit(lookupUnit));
            if (!typeDecl) {
                return resolveType(node, lookupUnit, false);
            }
            if (!typeDecl->isGeneric()) {
                error(applied->loc,
                      "type `" + describeTypeNode(applied, "<unknown type>") +
                          "` applies `[...]` arguments to a non-generic type",
                      "Remove the `[...]` arguments, or make the base type "
                      "generic before specializing it.");
            }
            if (applied->args.size() != typeDecl->typeParams.size()) {
                error(applied->loc,
                      "generic type argument count mismatch for `" +
                          toStdString(typeDecl->exportedName) + "`: expected " +
                          std::to_string(typeDecl->typeParams.size()) +
                          ", got " + std::to_string(applied->args.size()),
                      "Match the number of `[` `]` type arguments to the "
                      "generic type parameter list.");
            }
            std::vector<TypeClass *> argTypes;
            argTypes.reserve(applied->args.size());
            for (auto *arg : applied->args) {
                auto *argType = substituteAppliedStructTemplateType(
                    arg, genericArgs, arg ? arg->loc : loc, context, lookupUnit);
                if (!argType) {
                    error(arg ? arg->loc : loc,
                          "unknown type argument for `" +
                              describeTypeNode(applied, "<unknown type>") +
                              "`: " + describeTypeNode(arg, "void"));
                }
                argTypes.push_back(argType);
            }
            if (auto *templateOwnerUnit =
                    lookupUnit.ownerUnitForTypeDecl(typeDecl)) {
                return materializeVisibleAppliedStructType(
                    *typeDecl, std::move(argTypes), templateOwnerUnit);
            }
            auto appliedName =
                buildAppliedTypeName(toStdString(typeDecl->exportedName), argTypes);
            auto *structType = interface_->getOrCreateAppliedStructType(
                appliedName, typeDecl->declKind, typeDecl->exportedName,
                argTypes);
            if (structType) {
                structType->setAppliedTemplateInfo(typeDecl->exportedName,
                                                   argTypes, nullptr);
            }
            return structType;
        }
        if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
            auto *baseType = substituteAppliedStructTemplateType(
                qualified->base, genericArgs, loc, context, lookupUnit);
            return baseType ? interface_->getOrCreateConstType(baseType)
                            : nullptr;
        }
        if (auto *dynType = dynamic_cast<DynTypeNode *>(node)) {
            return resolveType(dynType, lookupUnit, false);
        }
        if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
            auto *baseType = substituteAppliedStructTemplateType(
                pointer->base, genericArgs, loc, context, lookupUnit);
            for (uint32_t i = 0; baseType && i < pointer->dim; ++i) {
                baseType = interface_->getOrCreatePointerType(baseType);
            }
            return baseType;
        }
        if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
            auto *elementType = substituteAppliedStructTemplateType(
                indexable->base, genericArgs, loc, context, lookupUnit);
            return elementType ? interface_->getOrCreateIndexablePointerType(
                                     elementType)
                               : nullptr;
        }
        if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
            auto *elementType = substituteAppliedStructTemplateType(
                array->base, genericArgs, loc, context, lookupUnit);
            return elementType ? interface_->getOrCreateArrayType(
                                     elementType, array->dim)
                               : nullptr;
        }
        if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
            std::vector<TypeClass *> itemTypes;
            itemTypes.reserve(tuple->items.size());
            for (auto *item : tuple->items) {
                itemTypes.push_back(substituteAppliedStructTemplateType(
                    item, genericArgs, item ? item->loc : loc, context,
                    lookupUnit));
            }
            return interface_->getOrCreateTupleType(itemTypes);
        }
        if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
            std::vector<TypeClass *> argTypes;
            std::vector<BindingKind> argBindingKinds;
            argTypes.reserve(func->args.size());
            argBindingKinds.reserve(func->args.size());
            for (auto *arg : func->args) {
                argBindingKinds.push_back(funcParamBindingKind(arg));
                argTypes.push_back(substituteAppliedStructTemplateType(
                    unwrapFuncParamType(arg), genericArgs,
                    arg ? arg->loc : loc, context, lookupUnit));
            }
            auto *retType = substituteAppliedStructTemplateType(
                func->ret, genericArgs, loc, context, lookupUnit);
            return interface_->getOrCreatePointerType(
                interface_->getOrCreateFunctionType(argTypes, retType,
                                                    std::move(argBindingKinds)));
        }
        return resolveType(node, lookupUnit, false);
    }

    StructType *materializeVisibleAppliedStructType(
        const ModuleInterface::TypeDecl &typeDecl,
        std::vector<TypeClass *> appliedTypeArgs,
        const CompilationUnit *templateOwnerUnit = nullptr) {
        if (!templateOwnerUnit) {
            templateOwnerUnit = unit_.ownerUnitForTypeDecl(&typeDecl);
        }

        const auto baseAppliedName =
            templateOwnerUnit == &unit_ ? toStdString(typeDecl.localName)
                                        : toStdString(typeDecl.exportedName);
        auto appliedName = buildAppliedTypeName(baseAppliedName, appliedTypeArgs);
        auto *structType = interface_->getOrCreateAppliedStructType(
            appliedName, typeDecl.declKind, typeDecl.exportedName,
            appliedTypeArgs);
        if (!structType) {
            return nullptr;
        }
        structType->setAppliedTemplateInfo(typeDecl.exportedName,
                                           appliedTypeArgs,
                                           templateOwnerUnit);

        auto [_, inserted] = materializingAppliedStructs_.insert(appliedName);
        if (!inserted) {
            return structType;
        }
        struct Guard {
            std::unordered_set<std::string> &active;
            std::string name;
            ~Guard() { active.erase(name); }
        } guard{materializingAppliedStructs_, appliedName};

        if (typeDecl.typeParams.size() != appliedTypeArgs.size()) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "generic struct `" + toStdString(typeDecl.localName) +
                    "` interface instance is missing concrete type arguments",
                "This looks like an applied-struct interface bug.");
        }

        std::unordered_map<std::string, TypeClass *> genericArgs;
        genericArgs.reserve(typeDecl.typeParams.size());
        for (std::size_t i = 0; i < typeDecl.typeParams.size(); ++i) {
            genericArgs.emplace(toStdString(typeDecl.typeParams[i].localName),
                                appliedTypeArgs[i]);
        }

        if (structType->isOpaque()) {
            auto *structDecl = findStructDeclInUnit(
                templateOwnerUnit, toStringRef(typeDecl.localName));
            if (!structDecl) {
                throw DiagnosticError(
                    DiagnosticError::Category::Internal,
                    "generic struct `" + toStdString(typeDecl.localName) +
                        "` is missing its template AST",
                    "This looks like a generic template registration bug.");
            }

            auto *body = dynamic_cast<AstStatList *>(structDecl->body);
            llvm::StringMap<StructType::ValueTy> members;
            llvm::StringMap<AccessKind> memberAccess;
            llvm::StringSet<> embeddedMembers;
            std::unordered_map<std::string, location> seenMembers;
            int index = 0;

            if (body) {
                for (auto *stmt : body->getBody()) {
                    auto *varDecl = dynamic_cast<AstVarDecl *>(stmt);
                    if (!varDecl) {
                        continue;
                    }
                    auto *fieldType = substituteAppliedStructTemplateType(
                        varDecl->typeNode, genericArgs, varDecl->loc,
                        "struct field `" + describeStructFieldSyntax(varDecl) +
                            "`",
                        *templateOwnerUnit);
                    if (!fieldType) {
                        error(varDecl->loc,
                              "unknown struct field type for `" +
                                  describeStructFieldSyntax(varDecl) + "`: " +
                                  describeTypeNode(varDecl->typeNode, "void"));
                    }
                    rejectBareFunctionType(
                        fieldType, varDecl->typeNode,
                        "unsupported bare function struct field type for `" +
                            describeStructFieldSyntax(varDecl) + "`",
                        varDecl->loc);
                    validateStructFieldType(structDecl, varDecl, fieldType);
                    validateEmbeddedStructField(structDecl, varDecl, fieldType);
                    insertStructMember(structDecl, varDecl, fieldType, members,
                                       memberAccess, embeddedMembers,
                                       seenMembers, index);
                }
            }

            structType->complete(members, memberAccess, embeddedMembers);
        }

        for (const auto &method : typeDecl.methodTemplates) {
            if (structType->getMethodType(toStringRef(method.localName))) {
                continue;
            }
            if (method.typeParams.size() > method.enclosingTypeParamCount) {
                continue;
            }
            std::vector<TypeClass *> argTypes;
            auto *selfPointee = interfaceMethodReceiverPointeeType(
                interface_, structType, method.receiverAccess);
            argTypes.push_back(interface_->getOrCreatePointerType(selfPointee));
            for (std::size_t i = 0; i < method.paramTypeNodes.size(); ++i) {
                auto *paramType = substituteAppliedStructTemplateType(
                    method.paramTypeNodes[i], genericArgs,
                    method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                             : location(),
                    "method parameter", *templateOwnerUnit);
                rejectBareFunctionType(
                    paramType, method.paramTypeNodes[i],
                    "unsupported bare function parameter type in `" +
                        toStdString(typeDecl.localName) + "." +
                        toStdString(method.localName) + "`",
                    method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                             : location());
                rejectOpaqueStructByValue(
                    paramType, method.paramTypeNodes[i],
                    method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                             : location(),
                    "parameter `" +
                        (i < method.paramNames.size()
                             ? toStdString(method.paramNames[i])
                             : std::string("<param>")) +
                        "` in method `" + toStdString(typeDecl.localName) +
                        "." + toStdString(method.localName) + "`");
                argTypes.push_back(paramType);
            }
            TypeClass *retType = nullptr;
            if (method.returnTypeNode) {
                retType = substituteAppliedStructTemplateType(
                    method.returnTypeNode, genericArgs,
                    method.returnTypeNode->loc, "method return type",
                    *templateOwnerUnit);
                rejectBareFunctionType(
                    retType, method.returnTypeNode,
                    "unsupported bare function return type for `" +
                        toStdString(typeDecl.localName) + "." +
                        toStdString(method.localName) + "`",
                    method.returnTypeNode->loc);
                rejectOpaqueStructByValue(
                    retType, method.returnTypeNode, method.returnTypeNode->loc,
                    "return type of method `" +
                        toStdString(typeDecl.localName) + "." +
                        toStdString(method.localName) + "`");
            }
            auto paramBindingKinds = method.paramBindingKinds;
            paramBindingKinds.insert(paramBindingKinds.begin(),
                                     BindingKind::Value);
            auto *funcType = interface_->getOrCreateFunctionType(
                argTypes, retType, paramBindingKinds);
            structType->addMethodType(toStringRef(method.localName), funcType,
                                      method.paramNames);
        }

        return structType;
    }

    TypeClass *materializeOpaqueAppliedStructIfNeeded(TypeClass *type) {
        if (!type) {
            return nullptr;
        }

        auto *qualified = type->as<ConstType>();
        auto *structType = asUnqualified<StructType>(type);
        if (!structType || !structType->isOpaque() ||
            !structType->isAppliedTemplateInstance()) {
            return type;
        }

        const CompilationUnit *templateOwnerUnit =
            structType->getAppliedTemplateOwnerUnit();
        const auto *typeDecl = findVisibleTypeDeclByTemplateName(
            toStringRef(structType->getAppliedTemplateName()),
            &templateOwnerUnit);
        if (!typeDecl || !typeDecl->isGeneric()) {
            return type;
        }

        auto *materialized = materializeVisibleAppliedStructType(
            *typeDecl, structType->getAppliedTypeArgs(), templateOwnerUnit);
        if (!qualified) {
            return materialized;
        }
        return materialized ? interface_->getOrCreateConstType(materialized)
                            : nullptr;
    }

    TypeClass *resolveAppliedType(AppliedTypeNode *applied,
                                  const CompilationUnit &lookupUnit) {
        auto *base = dynamic_cast<BaseTypeNode *>(applied ? applied->base : nullptr);
        auto appliedName = describeTypeNode(applied, "<unknown type>");
        if (!base) {
            return materializeOpaqueAppliedStructIfNeeded(
                interface_->findDerivedType(appliedName));
        }

        const auto *typeDecl = resolveVisibleTypeDecl(
            base, lookupUnit, interfaceForLookupUnit(lookupUnit));
        if (!typeDecl) {
            return materializeOpaqueAppliedStructIfNeeded(
                interface_->findDerivedType(appliedName));
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
            auto *argType = resolveType(arg, lookupUnit, false);
            if (!argType) {
                error(arg ? arg->loc : applied->loc,
                      "unknown type argument for `" + appliedName + "`: " +
                          describeTypeNode(arg, "void"));
            }
            argTypes.push_back(argType);
        }
        if (auto *templateOwnerUnit =
                lookupUnit.ownerUnitForTypeDecl(typeDecl)) {
            return materializeOpaqueAppliedStructIfNeeded(
                materializeVisibleAppliedStructType(*typeDecl,
                                                   std::move(argTypes),
                                                   templateOwnerUnit));
        }
        auto *structType = interface_->getOrCreateAppliedStructType(
            appliedName, typeDecl->declKind, typeDecl->exportedName, argTypes);
        if (structType) {
            structType->setAppliedTemplateInfo(typeDecl->exportedName,
                                               argTypes, nullptr);
        }
        return materializeOpaqueAppliedStructIfNeeded(structType);
    }

    TypeClass *resolveAppliedType(AppliedTypeNode *applied) {
        return resolveAppliedType(applied, unit_);
    }

    void validateAppliedTypeNode(
        AppliedTypeNode *applied, const std::unordered_set<std::string> &params,
        const location &loc, const std::string &context) {
        auto *base = dynamic_cast<BaseTypeNode *>(applied ? applied->base : nullptr);
        auto appliedName = describeTypeNode(applied, "<unknown type>");
        if (!base) {
            error(loc, "unknown type for " + context + ": " + appliedName,
                  "Type parameters are only visible inside the generic item "
                  "that declares them.");
        }

        const auto *typeDecl = resolveVisibleTypeDecl(base);
        if (!typeDecl) {
            error(loc, "unknown type for " + context + ": " + appliedName,
                  "Type parameters are only visible inside the generic item "
                  "that declares them.");
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
        for (auto *arg : applied->args) {
            validateGenericTypeNode(arg, params, arg ? arg->loc : loc,
                                    "type argument for `" + appliedName + "`");
        }
    }

    void validateGenericTypeNode(TypeNode *node,
                                 const std::unordered_set<std::string> &params,
                                 const location &loc,
                                 const std::string &context) {
        if (!node) {
            return;
        }
        validateTypeNodeLayout(node);
        if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
            validateGenericTypeNode(param->type, params, loc, context);
            return;
        }
        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            std::string moduleName;
            std::string memberName;
            auto rawName = baseTypeName(base);
            if (!splitBaseTypeName(base, moduleName, memberName) &&
                params.contains(rawName)) {
                return;
            }
            if (resolveType(node, false)) {
                return;
            }
            error(loc, "unknown type for " + context + ": " + rawName,
                  "Type parameters are only visible inside the generic item "
                  "that declares them.");
        }
        if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
            validateAppliedTypeNode(applied, params, loc, context);
            return;
        }
        if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
            validateGenericTypeNode(qualified->base, params, loc, context);
            return;
        }
        if (auto *dynType = dynamic_cast<DynTypeNode *>(node)) {
            validateGenericTypeNode(dynType->base, params, loc, context);
            return;
        }
        if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
            validateGenericTypeNode(pointer->base, params, loc, context);
            return;
        }
        if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
            validateGenericTypeNode(indexable->base, params, loc, context);
            return;
        }
        if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
            validateGenericTypeNode(array->base, params, loc, context);
            return;
        }
        if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
            for (auto *item : tuple->items) {
                validateGenericTypeNode(item, params, loc, context);
            }
            return;
        }
        if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
            for (auto *arg : func->args) {
                validateGenericTypeNode(arg, params, loc, context);
            }
            validateGenericTypeNode(func->ret, params, loc, context);
            return;
        }
        if (resolveType(node, false)) {
            return;
        }
        error(loc,
              "unknown type for " + context + ": " +
                  describeTypeNode(node, "void"),
              "Type parameters are only visible inside the generic item "
              "that declares them.");
    }

    void validateImportAliasConflict(AstStructDecl *structDecl) {
        const auto name = toStdString(structDecl->name);
        if (!unit_.importsModule(name)) {
            return;
        }
        error(structDecl->loc,
              "struct `" + name + "` conflicts with imported module alias `" +
                  name + "`",
              "Rename the struct so `" + name +
                  ".xxx` continues to refer to the imported module.");
    }

    void validateImportAliasConflict(AstFuncDecl *funcDecl) {
        const auto name = toStdString(funcDecl->name);
        if (!unit_.importsModule(name)) {
            return;
        }
        error(funcDecl->loc,
              "top-level function `" + name +
                  "` conflicts with imported module alias `" + name + "`",
              "Rename the function so `" + name +
                  ".xxx` continues to refer to the imported module.");
    }

    void validateImportAliasConflict(AstTraitDecl *traitDecl) {
        const auto name = toStdString(traitDecl->name);
        if (!unit_.importsModule(name)) {
            return;
        }
        error(traitDecl->loc,
              "trait `" + name + "` conflicts with imported module alias `" +
                  name + "`",
              "Rename the trait so `" + name +
                  ".xxx` continues to refer to the imported module.");
    }

    void validateImportAliasConflict(AstGlobalDecl *globalDecl) {
        const auto name = toStdString(globalDecl->getName());
        if (!unit_.importsModule(name)) {
            return;
        }
        error(globalDecl->loc,
              "global `" + name + "` conflicts with imported module alias `" +
                  name + "`",
              "Rename the global so `" + name +
                  ".xxx` continues to refer to the imported module.");
    }

    TypeClass *resolveType(TypeNode *node, bool validateLayout = true) {
        return resolveType(node, unit_, validateLayout);
    }

    static std::string describeResolvedTypeName(TypeClass *type) {
        return type ? toStdString(type->full_name) : std::string("void");
    }

    static std::string describeTraitMemberContext(AstTraitDecl *traitDecl) {
        return traitDecl ? "`" + toStdString(traitDecl->name) + "`"
                         : std::string("<trait>");
    }

    [[noreturn]] void errorUnsupportedTraitBodyStmt(AstTraitDecl *traitDecl,
                                                    AstNode *stmt) {
        const auto traitName = describeTraitMemberContext(traitDecl);
        if (auto *fieldDecl = dynamic_cast<AstVarDecl *>(stmt)) {
            error(fieldDecl->loc,
                  "trait " + traitName + " cannot declare field `" +
                      toStdString(fieldDecl->field) + "`",
                  "Trait v0 only allows method signatures inside trait "
                  "bodies.");
        }
        if (auto *varDef = dynamic_cast<AstVarDef *>(stmt)) {
            error(varDef->loc,
                  "trait " + traitName + " cannot declare local variable `" +
                      toStdString(varDef->getName()) + "`",
                  "Trait bodies describe interfaces only. Move executable "
                  "code out of the trait and keep only `def name(...)` "
                  "signatures here.");
        }
        if (auto *globalDecl = dynamic_cast<AstGlobalDecl *>(stmt)) {
            error(globalDecl->loc,
                  "trait " + traitName + " cannot declare global `" +
                      toStdString(globalDecl->getName()) + "`",
                  "Move globals to module scope. Trait v0 only allows method "
                  "signatures inside trait bodies.");
        }
        if (auto *structDecl = dynamic_cast<AstStructDecl *>(stmt)) {
            error(structDecl->loc,
                  "trait " + traitName + " cannot declare nested struct `" +
                      toStdString(structDecl->name) + "`",
                  "Move nested types to module scope. Trait bodies only allow "
                  "method signatures in trait v0.");
        }

        error(stmt ? stmt->loc : (traitDecl ? traitDecl->loc : location()),
              "trait " + traitName + " cannot contain executable statements",
              "Trait v0 only allows method signatures inside trait bodies.");
    }

    static std::string describeTraitImplContext(const ResolvedTraitRef &traitRef,
                                                const ResolvedSelfTypeRef &selfRef) {
        return "`" + toStdString(traitRef.resolvedName) + " for " +
               toStdString(selfRef.resolvedName) + "`";
    }

    std::vector<AstFuncDecl *> collectTraitImplBodyMethods(
        AstTraitImplDecl *traitImplDecl, const ResolvedTraitRef &traitRef,
        const ResolvedSelfTypeRef &selfRef) {
        std::vector<AstFuncDecl *> methods;
        auto *body = dynamic_cast<AstStatList *>(traitImplDecl ? traitImplDecl->body
                                                               : nullptr);
        if (!body) {
            return methods;
        }

        std::unordered_set<std::string> seenMethods;
        for (auto *stmt : body->getBody()) {
            auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt);
            if (!funcDecl) {
                error(stmt ? stmt->loc : traitImplDecl->loc,
                      "trait impl body " +
                          describeTraitImplContext(traitRef, selfRef) +
                          " can only contain method definitions",
                      "Keep only `def name(...) { ... }` entries inside the "
                      "impl body.");
            }
            if (funcDecl->hasTypeParams()) {
                error(funcDecl->loc,
                      "generic methods are not supported in trait impl body " +
                          describeTraitImplContext(traitRef, selfRef),
                      "Keep trait impl methods monomorphic for now.");
            }
            if (!funcDecl->hasBody()) {
                error(funcDecl->loc,
                      "trait impl method `" + toStdString(funcDecl->name) +
                          "` in " +
                          describeTraitImplContext(traitRef, selfRef) +
                          " must have a body",
                      "Write `def " + toStdString(funcDecl->name) +
                          "(...) { ... }` inside the impl body.");
            }
            if (!seenMethods.emplace(toStdString(funcDecl->name)).second) {
                error(funcDecl->loc,
                      "duplicate trait impl method `" +
                          toStdString(funcDecl->name) + "` in " +
                          describeTraitImplContext(traitRef, selfRef),
                      "Keep only one definition for each trait method inside "
                      "the impl body.");
            }
            methods.push_back(funcDecl);
        }
        return methods;
    }

    ResolvedTraitRef resolveTraitRef(AstNode *traitSyntax,
                                     const location &loc) {
        std::vector<std::string> segments;
        if (!collectDotLikeSegments(traitSyntax, segments) || segments.empty()) {
            error(loc, "invalid trait reference in impl declaration",
                  "Use a trait name like `Hash` or `dep.Hash` in "
                  "`impl Trait for Type { ... }`.");
        }

        if (segments.size() == 1) {
            if (const auto *traitDecl = interface_->findTrait(segments[0])) {
                return {traitDecl, traitDecl->exportedName, true};
            }
            error(loc, "unknown trait `" + segments[0] + "` in impl declaration",
                  "Declare the trait in this module or import the module that "
                  "defines it before writing the impl.");
        }

        if (segments.size() != 2) {
            error(loc,
                  "trait references only support directly imported module "
                  "members in trait v0",
                  "Use `Trait` or `dep.Trait`. Re-exported multi-hop trait "
                  "paths are not supported yet.");
        }

        const auto *imported = findImportedModuleForLookup(unit_, segments[0]);
        if (!imported || !imported->interface) {
            error(loc, "unknown imported module alias `" + segments[0] + "`",
                  "Add an explicit `import " + segments[0] +
                      "` before referring to `" + describeDotLikeSyntax(
                                                       traitSyntax, "<trait>") +
                      "`.");
        }

        auto lookup = imported->interface->lookupTopLevelName(segments[1]);
        if (!lookup.isTrait() || !lookup.traitDecl) {
            error(loc,
                  "unknown trait `" + describeDotLikeSyntax(traitSyntax,
                                                            "<trait>") +
                      "` in impl declaration",
                  "Only directly imported top-level traits are available "
                  "through `file.Trait`.");
        }
        return {lookup.traitDecl, lookup.traitDecl->exportedName, false};
    }

    ResolvedSelfTypeRef resolveImplSelfType(
        TypeNode *selfTypeNode,
        const std::vector<ModuleInterface::GenericParamDecl> &typeParams,
        const location &loc) {
        auto *base = rootSelfTypeBase(selfTypeNode);
        if (!base) {
            error(loc,
                  "trait impl self type must name a struct type",
                  "Write `impl Hash for Point` or "
                  "`impl[T Trait] Hash for Box[T]`, not pointers, arrays, "
                  "tuples, or function types.");
        }

        auto genericParamNames = collectGenericParamNames(typeParams);
        if (!genericParamNames.empty()) {
            validateGenericTypeNode(selfTypeNode, genericParamNames, loc,
                                    "trait impl self type");
        }

        const auto *typeDecl = resolveVisibleTypeDecl(base);
        if (!typeDecl) {
            auto *type = resolveType(selfTypeNode);
            auto *structType = type ? type->as<StructType>() : nullptr;
            if (!structType) {
                error(loc,
                      "trait impl self type must resolve to a struct: `" +
                          describeTypeNode(selfTypeNode, "<unknown type>") +
                          "`",
                      "Trait v0 currently supports impls for struct types "
                      "only.");
            }
            std::string moduleName;
            std::string memberName;
            const bool localToUnit =
                !splitBaseTypeName(base, moduleName, memberName);
            return {structType, nullptr,
                    string(structType->full_name), localToUnit,
                    dynamic_cast<BaseTypeNode *>(selfTypeNode) != nullptr};
        }

        auto *declStructType = typeDecl->type ? typeDecl->type->as<StructType>()
                                              : nullptr;
        if (!declStructType) {
            error(loc,
                  "trait impl self type must resolve to a struct: `" +
                      toStdString(
                          qualifySelfTypeSpelling(selfTypeNode, typeDecl)) +
                      "`",
                  "Trait v0 currently supports impls for struct types only.");
        }

        if (typeDecl->isGeneric() &&
            dynamic_cast<AppliedTypeNode *>(selfTypeNode) == nullptr) {
            error(loc,
                  "generic impl self type requires declaration-style type arguments: `" +
                      toStdString(typeDecl->exportedName) + "`",
                  "Write `impl[T Trait] Trait for Box[T]` or "
                  "`impl Trait for Box[i32]`.");
        }

        StructType *resolvedStructType = nullptr;
        if (!typeParams.empty()) {
            resolvedStructType = declStructType;
        } else if (dynamic_cast<AppliedTypeNode *>(selfTypeNode)) {
            auto *resolvedType = resolveType(selfTypeNode);
            resolvedStructType =
                resolvedType ? resolvedType->as<StructType>() : nullptr;
        } else {
            resolvedStructType = declStructType;
        }

        std::string moduleName;
        std::string memberName;
        const bool localToUnit =
            !splitBaseTypeName(base, moduleName, memberName);
        const bool concreteMethodValidation =
            dynamic_cast<BaseTypeNode *>(selfTypeNode) != nullptr &&
            !typeDecl->isGeneric();
        return {resolvedStructType, typeDecl,
                qualifySelfTypeSpelling(selfTypeNode, typeDecl), localToUnit,
                concreteMethodValidation};
    }

    static AccessKind inferMethodReceiverAccess(StructType *selfType,
                                                FuncType *methodType,
                                                const location &loc,
                                                llvm::StringRef methodName) {
        if (!selfType || !methodType || methodType->getArgTypes().empty()) {
            internalError(loc,
                          "trait impl validation is missing the implicit self "
                          "parameter for method `" +
                              methodName.str() + "`",
                          "This looks like a method interface bug.");
        }
        auto *selfPointeeType =
            getRawPointerPointeeType(methodType->getArgTypes().front());
        if (!selfPointeeType ||
            asUnqualified<StructType>(selfPointeeType) != selfType) {
            internalError(loc,
                          "trait impl validation found an invalid self "
                          "parameter for method `" +
                              methodName.str() + "`",
                          "This looks like a method interface bug.");
        }
        return selfPointeeType == selfType ? AccessKind::GetSet
                                           : AccessKind::GetOnly;
    }

    void validateTraitMethodMatch(const ModuleInterface::TraitDecl &traitDecl,
                                  const ModuleInterface::TraitMethodDecl &method,
                                  StructType *selfType,
                                  const location &implLoc) {
        auto *methodType = selfType->getMethodType(toStringRef(method.localName));
        const auto implLabel =
            "`" + describeResolvedType(selfType) + ": " +
            toStdString(traitDecl.exportedName) + "`";
        if (!methodType) {
            error(implLoc,
                  "impl " + implLabel + " is missing method `" +
                      toStdString(method.localName) + "`",
                  "Define `def " + toStdString(method.localName) +
                      "(...)` on `" + describeResolvedType(selfType) +
                      "` with the same signature as trait `" +
                      toStdString(traitDecl.exportedName) + "`.");
        }

        auto actualReceiverAccess = inferMethodReceiverAccess(
            selfType, methodType, implLoc, toStringRef(method.localName));
        if (actualReceiverAccess != method.receiverAccess) {
            error(
                implLoc,
                "impl " + implLabel + " has receiver access mismatch for `" +
                    toStdString(method.localName) + "`",
                "Trait `" + toStdString(traitDecl.exportedName) + "` expects `" +
                    accessKindKeyword(method.receiverAccess) + " def " +
                    toStdString(method.localName) + "`.");
        }

        const auto &argTypes = methodType->getArgTypes();
        const std::size_t actualParamCount =
            argTypes.empty() ? 0 : argTypes.size() - 1;
        if (actualParamCount != method.paramTypeSpellings.size()) {
            error(implLoc,
                  "impl " + implLabel + " has parameter count mismatch for `" +
                      toStdString(method.localName) + "`: expected " +
                      std::to_string(method.paramTypeSpellings.size()) +
                      ", got " + std::to_string(actualParamCount),
                  "Match the trait method signature exactly.");
        }

        for (std::size_t i = 0; i < actualParamCount; ++i) {
            const auto actualBindingKind = methodType->getArgBindingKind(i + 1);
            if (actualBindingKind != method.paramBindingKinds[i]) {
                error(implLoc,
                      "impl " + implLabel +
                          " has parameter binding mismatch for `" +
                          toStdString(method.localName) + "` at index " +
                          std::to_string(i),
                      "Trait `" + toStdString(traitDecl.exportedName) +
                          "` expects `" +
                          bindingKindKeyword(method.paramBindingKinds[i]) +
                          "` at that position.");
            }

            const auto actualTypeName =
                describeResolvedTypeName(argTypes[i + 1]);
            if (actualTypeName != method.paramTypeSpellings[i]) {
                error(implLoc,
                      "impl " + implLabel + " has parameter type mismatch for `" +
                          toStdString(method.localName) + "` at index " +
                          std::to_string(i) + ": expected `" +
                          toStdString(method.paramTypeSpellings[i]) + "`, got `" +
                          actualTypeName + "`",
                      "Match the trait method parameter types exactly.");
            }
        }

        const auto actualReturnTypeName =
            describeResolvedTypeName(methodType->getRetType());
        if (actualReturnTypeName != method.returnTypeSpelling) {
            error(implLoc,
                  "impl " + implLabel + " has return type mismatch for `" +
                      toStdString(method.localName) + "`: expected `" +
                      toStdString(method.returnTypeSpelling) + "`, got `" +
                      actualReturnTypeName + "`",
                  "Match the trait method return type exactly.");
        }
    }

    void validateTraitBodyMethodMatch(
        const ModuleInterface::TraitDecl &traitDecl,
        const ModuleInterface::TraitMethodDecl &method,
        const ModuleInterface::MethodTemplateDecl &bodyMethod,
        const ResolvedSelfTypeRef &selfRef, const location &implLoc) {
        const auto implLabel =
            "`" + toStdString(selfRef.resolvedName) + ": " +
            toStdString(traitDecl.exportedName) + "`";
        if (bodyMethod.receiverAccess != method.receiverAccess) {
            error(
                implLoc,
                "impl " + implLabel + " has receiver access mismatch for `" +
                    toStdString(method.localName) + "`",
                "Trait `" + toStdString(traitDecl.exportedName) + "` expects `" +
                    accessKindKeyword(method.receiverAccess) + " def " +
                    toStdString(method.localName) + "`.");
        }

        const std::size_t actualParamCount = bodyMethod.paramTypeSpellings.size();
        if (actualParamCount != method.paramTypeSpellings.size()) {
            error(implLoc,
                  "impl " + implLabel + " has parameter count mismatch for `" +
                      toStdString(method.localName) + "`: expected " +
                      std::to_string(method.paramTypeSpellings.size()) +
                      ", got " + std::to_string(actualParamCount),
                  "Match the trait method signature exactly.");
        }

        for (std::size_t i = 0; i < actualParamCount; ++i) {
            if (bodyMethod.paramBindingKinds[i] != method.paramBindingKinds[i]) {
                error(implLoc,
                      "impl " + implLabel +
                          " has parameter binding mismatch for `" +
                          toStdString(method.localName) + "` at index " +
                          std::to_string(i),
                      "Trait `" + toStdString(traitDecl.exportedName) +
                          "` expects `" +
                          bindingKindKeyword(method.paramBindingKinds[i]) +
                          "` at that position.");
            }

            if (bodyMethod.paramTypeSpellings[i] != method.paramTypeSpellings[i]) {
                error(implLoc,
                      "impl " + implLabel + " has parameter type mismatch for `" +
                          toStdString(method.localName) + "` at index " +
                          std::to_string(i) + ": expected `" +
                          toStdString(method.paramTypeSpellings[i]) + "`, got `" +
                          toStdString(bodyMethod.paramTypeSpellings[i]) + "`",
                      "Match the trait method parameter types exactly.");
            }
        }

        if (bodyMethod.returnTypeSpelling != method.returnTypeSpelling) {
            error(implLoc,
                  "impl " + implLabel + " has return type mismatch for `" +
                      toStdString(method.localName) + "`: expected `" +
                      toStdString(method.returnTypeSpelling) + "`, got `" +
                      toStdString(bodyMethod.returnTypeSpelling) + "`",
                  "Match the trait method return type exactly.");
        }
    }

    void checkVisibleTraitImplConflicts(
        const std::vector<ValidatedTraitImpl> &validatedImpls) {
        std::unordered_map<std::string, std::string> seenSources;

        auto makeKey = [](const string &traitName, const string &selfTypeName) {
            return toStdString(traitName) + "|" + toStdString(selfTypeName);
        };

        for (const auto &importedEntry : unit_.importedModules()) {
            const auto &alias = importedEntry.first;
            const auto &imported = importedEntry.second;
            if (!imported.interface) {
                continue;
            }
            for (const auto &implDecl : imported.interface->traitImpls()) {
                auto key = makeKey(implDecl.traitName, implDecl.selfTypeSpelling);
                auto source = "imported module `" + toStdString(alias) + "`";
                auto found = seenSources.find(key);
                if (found != seenSources.end()) {
                    error(unit_.syntaxTree() ? unit_.syntaxTree()->loc : location(),
                          "duplicate visible impl for trait `" +
                              toStdString(implDecl.traitName) + "` and type `" +
                              toStdString(implDecl.selfTypeSpelling) + "`",
                          "Only one visible impl is allowed for each "
                          "(Trait, Type) pair. Existing source: " +
                              found->second + ", new source: " + source + ".");
                }
                seenSources.emplace(std::move(key), std::move(source));
            }
        }

        for (const auto &entry : validatedImpls) {
            auto key = makeKey(entry.decl.traitName, entry.decl.selfTypeSpelling);
            auto found = seenSources.find(key);
            if (found != seenSources.end()) {
                error(entry.loc,
                      "duplicate visible impl for trait `" +
                          toStdString(entry.decl.traitName) + "` and type `" +
                          toStdString(entry.decl.selfTypeSpelling) + "`",
                      "Only one visible impl is allowed for each "
                      "(Trait, Type) pair. Existing source: " + found->second +
                      ".");
            }
            seenSources.emplace(
                std::move(key),
                "current module `" + toStdString(unit_.moduleName()) + "`");
        }
    }

    std::vector<ModuleInterface::TraitMethodDecl>
    collectTraitMethods(AstTraitDecl *traitDecl) {
        std::vector<ModuleInterface::TraitMethodDecl> methods;
        auto *body = dynamic_cast<AstStatList *>(traitDecl ? traitDecl->body
                                                           : nullptr);
        if (!body) {
            return methods;
        }

        std::unordered_map<std::string, location> seenMethods;

        for (auto *stmt : body->getBody()) {
            if (dynamic_cast<AstTagNode *>(stmt)) {
                continue;
            }
            auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt);
            if (!funcDecl) {
                errorUnsupportedTraitBodyStmt(traitDecl, stmt);
            }
            if (funcDecl->hasBody()) {
                error(funcDecl->loc,
                      "trait method `" + toStdString(funcDecl->name) +
                          "` cannot have a body in trait v0",
                      "Keep only the method signature inside the trait. "
                      "Put implementations in a separate impl body such as "
                      "`impl Hash for Point { ... }`.");
            }
            if (funcDecl->hasTypeParams()) {
                error(funcDecl->loc,
                      "generic methods are not supported in generic v0: `" +
                          toStdString(traitDecl->name) + "." +
                          toStdString(funcDecl->name) + "`",
                      "Use a top-level generic `def`, or keep trait v0 "
                      "methods monomorphic in the first implementation cut.");
            }
            auto inserted =
                seenMethods.emplace(toStdString(funcDecl->name), funcDecl->loc);
            if (!inserted.second) {
                error(funcDecl->loc,
                      "duplicate trait method `" +
                          toStdString(funcDecl->name) + "` in trait `" +
                          toStdString(traitDecl->name) + "`",
                      "Trait method names must be unique within the same "
                      "trait.");
            }

            ModuleInterface::TraitMethodDecl method;
            method.localName = funcDecl->name;
            method.receiverAccess = funcDecl->receiverAccess;
            method.paramNames = extractParamNames(funcDecl);
            method.paramBindingKinds = extractParamBindingKinds(funcDecl);
            if (funcDecl->args) {
                method.paramTypeSpellings.reserve(funcDecl->args->size());
                for (auto *arg : *funcDecl->args) {
                    auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
                    if (!varDecl) {
                        error(funcDecl->loc,
                              "invalid trait method parameter declaration in "
                              "`" +
                                  toStdString(funcDecl->name) + "`");
                    }
                    auto *paramType = resolveType(varDecl->typeNode);
                    if (!paramType) {
                        error(varDecl->loc,
                              "unknown type for trait method parameter `" +
                                  toStdString(varDecl->field) + "` in `" +
                                  toStdString(funcDecl->name) + "`: " +
                                  describeTypeNode(varDecl->typeNode, "void"));
                    }
                    rejectBareFunctionType(
                        paramType, varDecl->typeNode,
                        "unsupported bare function trait parameter type for `" +
                            toStdString(varDecl->field) + "` in `" +
                            toStdString(funcDecl->name) + "`",
                        varDecl->loc);
                    rejectOpaqueStructByValue(
                        paramType, varDecl->typeNode, varDecl->loc,
                        "parameter `" + toStdString(varDecl->field) +
                            "` in trait method `" +
                            toStdString(funcDecl->name) + "`");
                    method.paramTypeSpellings.push_back(paramType->full_name);
                }
            }
            if (funcDecl->retType) {
                auto *retType = resolveType(funcDecl->retType);
                if (!retType) {
                    error(funcDecl->loc,
                          "unknown return type for trait method `" +
                              toStdString(funcDecl->name) + "`: " +
                              describeTypeNode(funcDecl->retType, "void"));
                }
                rejectBareFunctionType(
                    retType, funcDecl->retType,
                    "unsupported bare function return type for trait method `" +
                        toStdString(funcDecl->name) + "`",
                    funcDecl->loc);
                rejectOpaqueStructByValue(
                    retType, funcDecl->retType, funcDecl->loc,
                    "return type of trait method `" +
                        toStdString(funcDecl->name) + "`");
                method.returnTypeSpelling = retType->full_name;
            } else {
                method.returnTypeSpelling = "void";
            }
            methods.push_back(std::move(method));
        }
        return methods;
    }

    void collectTopLevelLists(AstNode *root) {
        auto *program = dynamic_cast<AstProgram *>(root);
        auto *body =
            dynamic_cast<AstStatList *>(program ? program->body : root);
        if (!body) {
            return;
        }
        for (auto *stmt : body->getBody()) {
            if (auto *structDecl = dynamic_cast<AstStructDecl *>(stmt)) {
                validateImportAliasConflict(structDecl);
                validateStructDeclShape(structDecl);
                recordTopLevelDeclName(
                    topLevelDecls_, toStdString(structDecl->name),
                    TopLevelDeclKind::StructType, structDecl->loc);
                structDecls_.push_back(structDecl);
            } else if (auto *traitDecl = dynamic_cast<AstTraitDecl *>(stmt)) {
                validateImportAliasConflict(traitDecl);
                recordTopLevelDeclName(topLevelDecls_,
                                       toStdString(traitDecl->name),
                                       TopLevelDeclKind::Trait,
                                       traitDecl->loc);
                traitDecls_.push_back(traitDecl);
            } else if (auto *traitImplDecl =
                           dynamic_cast<AstTraitImplDecl *>(stmt)) {
                traitImplDecls_.push_back(traitImplDecl);
            } else if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt)) {
                validateImportAliasConflict(funcDecl);
                recordTopLevelDeclName(
                    topLevelDecls_, toStdString(funcDecl->name),
                    TopLevelDeclKind::Function, funcDecl->loc);
                funcDecls_.push_back(funcDecl);
            } else if (auto *globalDecl = dynamic_cast<AstGlobalDecl *>(stmt)) {
                validateImportAliasConflict(globalDecl);
                recordTopLevelDeclName(
                    topLevelDecls_, toStdString(globalDecl->getName()),
                    TopLevelDeclKind::Global, globalDecl->loc);
                globalDecls_.push_back(globalDecl);
            }
        }
    }

    void declareStructs() {
        for (auto *structDecl : structDecls_) {
            auto *structType = interface_->declareStructType(
                toStdString(structDecl->name), structDecl->declKind,
                collectGenericParams(structDecl->typeParams,
                                     GenericParamContext::StructDecl));
            if (structType) {
                localStructDecls_[structType] = structDecl;
                structCompletionStates_[structDecl] =
                    StructCompletionState::Pending;
            }
        }
    }

    void declareImportedModules() {
        for (const auto &entry : unit_.importedModules()) {
            const auto &imported = entry.second;
            interface_->declareImportedModule(entry.first, imported.moduleKey,
                                              imported.moduleName,
                                              imported.interface);
        }
    }

    void declareTraitNames() {
        for (auto *traitDecl : traitDecls_) {
            if (!interface_->declareTrait(toStdString(traitDecl->name), {})) {
                error(traitDecl->loc,
                      "duplicate trait `" + toStdString(traitDecl->name) + "`",
                      "Choose a distinct top-level trait name in this "
                      "module.");
            }
        }
    }

    void defineTraitMethods() {
        for (auto *traitDecl : traitDecls_) {
            auto methods = collectTraitMethods(traitDecl);
            if (!interface_->defineTraitMethods(toStdString(traitDecl->name),
                                                std::move(methods))) {
                internalError(traitDecl->loc,
                              "trait method collection ran before trait "
                              "declaration registration",
                              "This looks like a trait interface collection "
                              "ordering bug.");
            }
        }
    }

    std::vector<ValidatedTraitImpl> validateTraitImpls() {
        std::vector<ValidatedTraitImpl> validated;
        validated.reserve(traitImplDecls_.size());

        for (auto *traitImplDecl : traitImplDecls_) {
            auto implTypeParams = collectGenericParams(
                traitImplDecl->typeParams,
                GenericParamContext::TraitImplHeader);
            auto traitRef = resolveTraitRef(traitImplDecl->trait,
                                            traitImplDecl->loc);
            auto selfRef = resolveImplSelfType(traitImplDecl->selfType,
                                               implTypeParams,
                                               traitImplDecl->loc);
            if (!traitRef.localToUnit && !selfRef.localToUnit) {
                error(traitImplDecl->loc,
                      "impl `" + toStdString(traitRef.resolvedName) + " for " +
                          toStdString(selfRef.resolvedName) +
                          "` violates the trait orphan rule",
                      "At least one side of an impl must be defined in the "
                      "current module.");
            }

            if (traitImplDecl->hasBody()) {
                auto bodyMethods =
                    collectTraitImplBodyMethods(traitImplDecl, traitRef, selfRef);
                auto bodyMethodInterfaces = collectTraitImplBodyMethodInterfaces(
                    bodyMethods, selfRef.structType, implTypeParams);
                std::unordered_map<std::string, AstFuncDecl *> bodyMethodMap;
                std::unordered_map<std::string,
                                   const ModuleInterface::MethodTemplateDecl *>
                    bodyMethodInterfaceMap;
                for (auto *methodDecl : bodyMethods) {
                    bodyMethodMap.emplace(toStdString(methodDecl->name),
                                          methodDecl);
                    if (traitRef.decl->findMethod(
                            toStdString(methodDecl->name)) == nullptr) {
                        error(methodDecl->loc,
                              "trait impl body " +
                                  describeTraitImplContext(traitRef, selfRef) +
                                  " defines unknown method `" +
                                  toStdString(methodDecl->name) + "`",
                              "Only methods declared by trait `" +
                                  toStdString(traitRef.resolvedName) +
                                  "` may appear in this impl body.");
                    }
                }
                for (const auto &methodDecl : bodyMethodInterfaces) {
                    bodyMethodInterfaceMap.emplace(toStdString(methodDecl.localName),
                                                   &methodDecl);
                }
                for (const auto &method : traitRef.decl->methods) {
                    if (bodyMethodMap.find(toStdString(method.localName)) ==
                        bodyMethodMap.end()) {
                        error(traitImplDecl->loc,
                              "impl `" + toStdString(traitRef.resolvedName) +
                                  " for " + toStdString(selfRef.resolvedName) +
                                  "` is missing method `" +
                                  toStdString(method.localName) + "`",
                              "Define `def " + toStdString(method.localName) +
                                  "(...) { ... }` inside this impl body.");
                    }
                    auto foundInterface =
                        bodyMethodInterfaceMap.find(toStdString(method.localName));
                    if (foundInterface == bodyMethodInterfaceMap.end() ||
                        !foundInterface->second) {
                        internalError(traitImplDecl->loc,
                                      "trait impl body method metadata is "
                                      "missing during validation",
                                      "This looks like a trait impl interface "
                                      "collection bug.");
                    }
                    validateTraitBodyMethodMatch(*traitRef.decl, method,
                                                 *foundInterface->second, selfRef,
                                                 traitImplDecl->loc);
                }

                validated.push_back(ValidatedTraitImpl{
                    ModuleInterface::TraitImplDecl{
                        selfRef.resolvedName, traitImplDecl->selfType,
                        traitRef.resolvedName, traitImplDecl->hasBody(),
                        std::move(implTypeParams), traitImplDecl,
                        std::move(bodyMethodInterfaces)},
                    traitImplDecl->loc});
                continue;
            } else if (selfRef.concreteMethodValidation) {
                for (const auto &method : traitRef.decl->methods) {
                    validateTraitMethodMatch(*traitRef.decl, method,
                                             selfRef.structType,
                                             traitImplDecl->loc);
                }
            }

            validated.push_back(ValidatedTraitImpl{
                ModuleInterface::TraitImplDecl{selfRef.resolvedName,
                                               traitImplDecl->selfType,
                                               traitRef.resolvedName,
                                               traitImplDecl->hasBody(),
                                               std::move(implTypeParams),
                                               traitImplDecl, {}},
                traitImplDecl->loc});
        }

        return validated;
    }

    void declareValidatedTraitImpls(
        const std::vector<ValidatedTraitImpl> &validatedImpls) {
        for (const auto &implDecl : validatedImpls) {
            interface_->declareTraitImpl(implDecl.decl.selfTypeSpelling,
                                         implDecl.decl.selfTypeNode,
                                         implDecl.decl.traitName,
                                         implDecl.decl.hasBody,
                                         implDecl.decl.typeParams,
                                         implDecl.decl.syntaxDecl,
                                         implDecl.decl.bodyMethods);
        }
    }

    const ModuleInterface::TypeDecl *requireStructTypeDecl(
        AstStructDecl *structDecl) {
        auto *typeDecl =
            structDecl ? interface_->findType(toStdString(structDecl->name))
                       : nullptr;
        if (!typeDecl || !typeDecl->type || !typeDecl->type->as<StructType>()) {
            error(structDecl ? structDecl->loc : location(),
                  "failed to declare struct interface");
        }
        return typeDecl;
    }

    [[noreturn]] void errorRecursiveByValueStructCycle(AstStructDecl *structDecl,
                                                       AstVarDecl *fieldDecl) {
        auto structName =
            structDecl ? toStdString(structDecl->name) : std::string("<struct>");
        error(fieldDecl ? fieldDecl->loc : location(),
              "struct `" + structName +
                  "` forms a recursive by-value cycle through field `" +
                  describeStructFieldSyntax(fieldDecl) + "`",
              "Use pointers for recursive links instead of embedding the "
              "recursive struct by value.");
    }

    void ensureStructFieldDependenciesCompleted(TypeClass *fieldType,
                                                AstStructDecl *structDecl,
                                                AstVarDecl *fieldDecl) {
        if (!fieldType) {
            return;
        }
        if (auto *qualified = fieldType->as<ConstType>()) {
            ensureStructFieldDependenciesCompleted(qualified->getBaseType(),
                                                   structDecl, fieldDecl);
            return;
        }
        if (auto *structType = fieldType->as<StructType>()) {
            if (structType->isOpaqueDecl()) {
                auto typeName = describeTypeNode(fieldDecl ? fieldDecl->typeNode
                                                           : nullptr,
                                                 "void");
                error(fieldDecl ? fieldDecl->loc : location(),
                      "opaque struct `" + typeName +
                          "` cannot be used by value in struct field `" +
                          describeStructFieldSyntax(fieldDecl) + "`",
                      "Use `" + typeName +
                          "*` instead. Opaque structs are only supported "
                          "behind pointers.");
            }
            if (!structType->isOpaque()) {
                return;
            }
            auto found = localStructDecls_.find(structType);
            if (found == localStructDecls_.end()) {
                return;
            }
            auto *dependencyDecl = found->second;
            auto state =
                structCompletionStates_[dependencyDecl];
            if (state == StructCompletionState::Completed) {
                return;
            }
            if (state == StructCompletionState::Completing) {
                errorRecursiveByValueStructCycle(structDecl, fieldDecl);
            }
            completeStruct(dependencyDecl);
            return;
        }
        if (fieldType->as<PointerType>() || fieldType->as<IndexablePointerType>() ||
            fieldType->as<FuncType>() || fieldType->as<DynTraitType>() ||
            fieldType->as<BaseType>() || fieldType->as<AnyType>()) {
            return;
        }
        if (auto *arrayType = fieldType->as<ArrayType>()) {
            ensureStructFieldDependenciesCompleted(arrayType->getElementType(),
                                                   structDecl, fieldDecl);
            return;
        }
        if (auto *tupleType = fieldType->as<TupleType>()) {
            for (auto *itemType : tupleType->getItemTypes()) {
                ensureStructFieldDependenciesCompleted(itemType, structDecl,
                                                       fieldDecl);
            }
        }
    }

    void completeStruct(AstStructDecl *structDecl) {
        if (!structDecl) {
            return;
        }

        auto state = structCompletionStates_[structDecl];
        if (state == StructCompletionState::Completed) {
            return;
        }
        if (state == StructCompletionState::Completing) {
            internalError(structDecl->loc,
                          "recursive struct completion escaped dependency "
                          "validation",
                          "This looks like a struct completion ordering bug.");
        }

        auto *typeDecl = requireStructTypeDecl(structDecl);
        auto *structType = typeDecl->type->as<StructType>();
        if (!structType || !structType->isOpaque()) {
            structCompletionStates_[structDecl] =
                StructCompletionState::Completed;
            return;
        }

        auto *body = dynamic_cast<AstStatList *>(structDecl->body);
        if (!body) {
            structCompletionStates_[structDecl] =
                StructCompletionState::Completed;
            return;
        }

        structCompletionStates_[structDecl] = StructCompletionState::Completing;

        auto genericParams = collectGenericParams(
            structDecl->typeParams, GenericParamContext::StructDecl);
        if (!genericParams.empty()) {
            auto genericParamNames = collectGenericParamNames(genericParams);
            for (auto *stmt : body->getBody()) {
                auto *varDecl = dynamic_cast<AstVarDecl *>(stmt);
                if (!varDecl) {
                    continue;
                }
                validateGenericTypeNode(
                    varDecl->typeNode, genericParamNames, varDecl->loc,
                    "struct field `" + describeStructFieldSyntax(varDecl) +
                        "`");
            }
            structCompletionStates_[structDecl] =
                StructCompletionState::Completed;
            return;
        }

        llvm::StringMap<StructType::ValueTy> members;
        llvm::StringMap<AccessKind> memberAccess;
        llvm::StringSet<> embeddedMembers;
        std::unordered_map<std::string, location> seenMembers;
        int index = 0;
        for (auto *stmt : body->getBody()) {
            auto *varDecl = dynamic_cast<AstVarDecl *>(stmt);
            if (!varDecl) {
                continue;
            }
            if (varDecl->bindingKind == BindingKind::Ref) {
                error(varDecl->loc,
                      "struct fields cannot use `ref` binding for `" +
                          describeStructFieldSyntax(varDecl) + "`",
                      "Store an explicit pointer type instead. Struct "
                      "fields must be value or pointer-like storage.");
            }
            auto *fieldType = resolveType(varDecl->typeNode);
            if (!fieldType) {
                error(varDecl->loc,
                      "unknown struct field type for `" +
                          describeStructFieldSyntax(varDecl) + "`: " +
                          describeTypeNode(varDecl->typeNode, "void"));
            }
            ensureStructFieldDependenciesCompleted(fieldType, structDecl,
                                                   varDecl);
            rejectBareFunctionType(
                fieldType, varDecl->typeNode,
                "unsupported bare function struct field type for `" +
                    describeStructFieldSyntax(varDecl) + "`",
                varDecl->loc);
            validateStructFieldType(structDecl, varDecl, fieldType);
            validateEmbeddedStructField(structDecl, varDecl, fieldType);
            insertStructMember(structDecl, varDecl, fieldType, members,
                               memberAccess, embeddedMembers, seenMembers,
                               index);
        }

        structType->complete(members, memberAccess, embeddedMembers);
        structCompletionStates_[structDecl] = StructCompletionState::Completed;
    }

    void completeStructs() {
        for (auto *structDecl : structDecls_) {
            completeStruct(structDecl);
        }
    }

    CollectedFunctionInterface collectFunctionInterface(
        AstFuncDecl *node, StructType *methodParent,
        const std::vector<ModuleInterface::GenericParamDecl> *scopedTypeParams =
            nullptr) {
        CollectedFunctionInterface collected;
        validateFunctionReceiverAccess(node, methodParent);
        collected.paramNames = extractParamNames(node);
        collected.paramBindingKinds =
            extractParamBindingKinds(node, methodParent != nullptr);
        if (scopedTypeParams) {
            collected.typeParams = *scopedTypeParams;
        }
        if (node && node->hasTypeParams()) {
            auto ownTypeParams = collectGenericParams(
                node->typeParams, GenericParamContext::FunctionDecl);
            collected.typeParams.insert(collected.typeParams.end(),
                                        ownTypeParams.begin(),
                                        ownTypeParams.end());
        }
        auto genericParamNames = collectGenericParamNames(collected.typeParams);

        if (!genericParamNames.empty()) {
            if (node->args) {
                collected.paramTypeSpellings.reserve(node->args->size());
                collected.paramTypeNodes.reserve(node->args->size());
                for (auto *arg : *node->args) {
                    auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
                    if (!varDecl) {
                        error(node->loc,
                              "invalid function parameter declaration in `" +
                                  toStdString(node->name) + "`");
                    }
                    validateGenericTypeNode(
                        varDecl->typeNode, genericParamNames, varDecl->loc,
                        "function parameter `" +
                            toStdString(varDecl->field) + "` in `" +
                            toStdString(node->name) + "`");
                    collected.paramTypeNodes.push_back(varDecl->typeNode);
                    collected.paramTypeSpellings.push_back(
                        describeTypeNode(varDecl->typeNode, "void"));
                }
            }
            if (node->retType) {
                validateGenericTypeNode(node->retType, genericParamNames,
                                        node->loc,
                                        "function `" +
                                            toStdString(node->name) +
                                            "` return type");
                collected.returnTypeNode = node->retType;
                collected.returnTypeSpelling =
                    describeTypeNode(node->retType, "void");
            }
            return collected;
        }

        std::vector<TypeClass *> argTypes;
        if (methodParent) {
            argTypes.push_back(interface_->getOrCreatePointerType(
                interfaceMethodReceiverPointeeType(interface_, methodParent,
                                                   node->receiverAccess)));
        }
        if (node->args) {
            for (auto *arg : *node->args) {
                auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
                if (!varDecl) {
                    error(node->loc,
                          "invalid function parameter declaration in `" +
                              toStdString(node->name) + "`");
                }
                auto *argType = resolveType(varDecl->typeNode);
                if (!argType) {
                    error(varDecl->loc,
                          "unknown type for function parameter `" +
                              toStdString(varDecl->field) + "` in `" +
                              toStdString(node->name) + "`: " +
                              describeTypeNode(varDecl->typeNode, "void"));
                }
                rejectBareFunctionType(
                    argType, varDecl->typeNode,
                    "unsupported bare function parameter type for `" +
                        toStdString(varDecl->field) + "` in `" +
                        toStdString(node->name) + "`",
                    varDecl->loc);
                rejectOpaqueStructByValue(
                    argType, varDecl->typeNode, varDecl->loc,
                    "parameter `" + toStdString(varDecl->field) +
                        "` in function `" + toStdString(node->name) + "`");
                argTypes.push_back(argType);
                collected.paramTypeNodes.push_back(varDecl->typeNode);
                collected.paramTypeSpellings.push_back(argType->full_name);
            }
        }

        TypeClass *retType = nullptr;
        if (node->retType) {
            collected.returnTypeNode = node->retType;
            retType = resolveType(node->retType);
            if (!retType) {
                error(node->loc, "unknown return type for function `" +
                                     toStdString(node->name) + "`: " +
                                     describeTypeNode(node->retType, "void"));
            }
            rejectBareFunctionType(
                retType, node->retType,
                "unsupported bare function return type for `" +
                    toStdString(node->name) + "`",
                node->loc);
            rejectOpaqueStructByValue(
                retType, node->retType, node->loc,
                "return type of function `" + toStdString(node->name) + "`");
            collected.returnTypeSpelling = retType->full_name;
        }

        validateExternCFunctionSignature(node, methodParent, argTypes, retType);
        collected.type = interface_->getOrCreateFunctionType(
            argTypes, retType, collected.paramBindingKinds, node->abiKind);
        return collected;
    }

    std::vector<ModuleInterface::MethodTemplateDecl>
    collectTraitImplBodyMethodInterfaces(
        const std::vector<AstFuncDecl *> &bodyMethods, StructType *selfType,
        const std::vector<ModuleInterface::GenericParamDecl> &implTypeParams) {
        std::vector<ModuleInterface::MethodTemplateDecl> methods;
        methods.reserve(bodyMethods.size());
        for (auto *funcDecl : bodyMethods) {
            auto collected =
                collectFunctionInterface(funcDecl, selfType, &implTypeParams);
            auto paramBindingKinds = std::move(collected.paramBindingKinds);
            if (!paramBindingKinds.empty()) {
                paramBindingKinds.erase(paramBindingKinds.begin());
            }
            methods.push_back(ModuleInterface::MethodTemplateDecl{
                funcDecl->name,
                funcDecl->receiverAccess,
                std::move(collected.paramNames),
                std::move(paramBindingKinds),
                std::move(collected.paramTypeNodes),
                std::move(collected.paramTypeSpellings),
                collected.returnTypeNode,
                std::move(collected.returnTypeSpelling),
                std::move(collected.typeParams),
                implTypeParams.size(),
                funcDecl,
            });
        }
        return methods;
    }

    void declareFunctions() {
        for (auto *structDecl : structDecls_) {
            auto *typeDecl =
                interface_->findType(toStdString(structDecl->name));
            auto *structType =
                typeDecl ? typeDecl->type->as<StructType>() : nullptr;
            auto *body = dynamic_cast<AstStatList *>(structDecl->body);
            if (!structType || !body) {
                continue;
            }
            for (auto *stmt : body->getBody()) {
                auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt);
                if (!funcDecl) {
                    continue;
                }
                auto collected = collectFunctionInterface(
                    funcDecl, structType,
                    typeDecl ? &typeDecl->typeParams : nullptr);
                auto *funcType = collected.type;
                if (!funcType) {
                    if (collected.isGeneric() && typeDecl) {
                        auto methodParamBindingKinds =
                            std::move(collected.paramBindingKinds);
                        if (!methodParamBindingKinds.empty()) {
                            methodParamBindingKinds.erase(
                                methodParamBindingKinds.begin());
                        }
                        interface_->declareStructMethodTemplate(
                            toStdString(structDecl->name),
                            ModuleInterface::MethodTemplateDecl{
                                funcDecl->name,
                                funcDecl->receiverAccess,
                                std::move(collected.paramNames),
                                std::move(methodParamBindingKinds),
                                std::move(collected.paramTypeNodes),
                                std::move(collected.paramTypeSpellings),
                                collected.returnTypeNode,
                                std::move(collected.returnTypeSpelling),
                                std::move(collected.typeParams),
                                typeDecl ? typeDecl->typeParams.size() : 0,
                            });
                    }
                    continue;
                }
                if (structType->getMethodType(llvm::StringRef(
                        funcDecl->name.tochara(), funcDecl->name.size())) ==
                    nullptr) {
                    structType->addMethodType(
                        llvm::StringRef(funcDecl->name.tochara(),
                                        funcDecl->name.size()),
                        funcType, extractParamNames(funcDecl));
                }
            }
        }

        for (auto *funcDecl : funcDecls_) {
            auto collected = collectFunctionInterface(funcDecl, nullptr);
            interface_->declareFunction(toStdString(funcDecl->name),
                                        collected.type,
                                        std::move(collected.paramNames),
                                        std::move(collected.paramBindingKinds),
                                        std::move(collected.paramTypeNodes),
                                        std::move(collected.paramTypeSpellings),
                                        collected.returnTypeNode,
                                        std::move(collected.returnTypeSpelling),
                                        std::move(collected.typeParams));
        }
    }

    void declareGlobals() {
        for (auto *globalDecl : globalDecls_) {
            TypeClass *globalType = nullptr;
            if (globalDecl->hasTypeNode()) {
                globalType = resolveType(globalDecl->getTypeNode());
                if (!globalType) {
                    error(globalDecl->loc,
                          "unknown type for global `" +
                              toStdString(globalDecl->getName()) + "`: " +
                              describeTypeNode(globalDecl->getTypeNode(),
                                               "void"));
                }
                rejectBareFunctionType(
                    globalType, globalDecl->getTypeNode(),
                    "unsupported bare function global type for `" +
                        toStdString(globalDecl->getName()) + "`",
                    globalDecl->loc);
                rejectOpaqueStructByValue(
                    globalType, globalDecl->getTypeNode(), globalDecl->loc,
                    "global `" + toStdString(globalDecl->getName()) + "`");
            } else {
                globalType = inferStaticLiteralInitializerType(
                    interface_, globalDecl->getInitVal());
                if (!globalType) {
                    if (auto *constant =
                            dynamic_cast<AstConst *>(globalDecl->getInitVal());
                        constant &&
                        constant->getType() == AstConst::Type::NULLPTR) {
                        error(globalDecl->loc,
                              "global `" + toStdString(globalDecl->getName()) +
                                  "` cannot infer a type from `null`",
                              "Add an explicit pointer type, for example "
                              "`global " +
                                  toStdString(globalDecl->getName()) +
                                  " u8* = null`.");
                    }
                    error(globalDecl->loc,
                          "global `" + toStdString(globalDecl->getName()) +
                              "` requires a statically typed initializer for "
                              "inference",
                          "This first version infers global types only from "
                          "literal initializers such as numbers, booleans, "
                          "chars, and strings. Add an explicit type for other "
                          "cases.");
                }
            }

            const auto globalName = toStdString(globalDecl->getName());
            if (!interface_->declareGlobal(globalName, globalType,
                                           globalDecl->isExtern())) {
                error(globalDecl->loc, "duplicate global `" + globalName + "`",
                      "Choose a distinct top-level global name in this "
                      "module.");
            }
        }
    }

public:
    explicit InterfaceCollector(CompilationUnit &unit)
        : unit_(unit), interface_(unit.interface()) {
        assert(interface_);
    }

    void collect() {
        interface_->clear();
        declareImportedModules();
        collectTopLevelLists(unit_.syntaxTree());
        declareTraitNames();
        declareStructs();
        completeStructs();
        defineTraitMethods();
        declareGlobals();
        declareFunctions();
        auto validatedTraitImpls = validateTraitImpls();
        checkVisibleTraitImplConflicts(validatedTraitImpls);
        declareValidatedTraitImpls(validatedTraitImpls);
        interface_->markCollected();
    }
};

void
ensureUnitInterfaceCollected(CompilationUnit &unit) {
    if (unit.interfaceCollected()) {
        return;
    }
    moduleinterface_impl::InterfaceCollector(unit).collect();
}

Function *
materializeDeclaredFunction(Scope &scope, TypeTable *typeMgr,
                            FuncType *funcType, llvm::StringRef llvmName,
                            std::vector<string> paramNames = {},
                            bool hasImplicitSelf = false,
                            const CompilationUnit *unit = nullptr) {
    auto *existing = scope.getObj(llvmName);
    if (existing) {
        auto *func = existing->as<Function>();
        auto *existingType = func ? func->getType()->as<FuncType>() : nullptr;
        if (!func || existingType != funcType ||
            func->hasImplicitSelf() != hasImplicitSelf) {
            reportFunctionConflict(unit, llvmName, existingType, funcType);
        }
        return func;
    }
    auto *expectedLLVMType =
        getFunctionAbiLLVMType(*typeMgr, funcType, hasImplicitSelf);
    if (auto *existingLLVM = typeMgr->getModule().getFunction(llvmName);
        existingLLVM && existingLLVM->getFunctionType() != expectedLLVMType) {
        reportFunctionConflict(unit, llvmName, nullptr, funcType);
    }
    auto *llvmFunc = llvm::Function::Create(
        expectedLLVMType,
        llvm::Function::ExternalLinkage, llvm::Twine(llvmName),
        typeMgr->getModule());
    annotateFunctionAbi(*llvmFunc, funcType->getAbiKind());
    auto *func = new Function(llvmFunc, funcType, std::move(paramNames),
                              hasImplicitSelf);
    scope.addObj(llvmName, func);
    return func;
}

std::string
chooseFunctionRuntimeName(Scope &scope,
                          const ModuleInterface::FunctionDecl &functionDecl,
                          bool exportNamespace) {
    auto runtimeName =
        exportNamespace ? toStdString(functionDecl.symbolName)
                        : toStdString(functionDecl.localName);
    if (exportNamespace || functionDecl.abiKind == AbiKind::C) {
        return runtimeName;
    }

    auto *existing = scope.getObj(llvm::StringRef(runtimeName));
    if (!existing) {
        return runtimeName;
    }

    auto *existingFunc = dynamic_cast<Function *>(existing);
    auto *existingType =
        existingFunc ? dynamic_cast<FuncType *>(existingFunc->getType())
                     : nullptr;
    auto *llvmFunc = existingFunc
                         ? llvm::dyn_cast_or_null<llvm::Function>(
                               existingFunc->getllvmValue())
                         : nullptr;
    if (existingFunc && existingType &&
        existingType->getAbiKind() == functionDecl.abiKind && llvmFunc &&
        llvmFunc->getName() == llvm::StringRef(runtimeName)) {
        return runtimeName;
    }

    return toStdString(functionDecl.symbolName);
}

Object *
materializeDeclaredGlobal(Scope &scope, TypeTable *typeMgr, TypeClass *type,
                          llvm::StringRef llvmName,
                          const CompilationUnit *unit = nullptr) {
    if (!type) {
        internalError("failed to materialize global declaration `" +
                          llvmName.str() + "` without a type",
                      "Global interfaces should only contain resolved types.");
    }

    if (auto *existing = scope.getObj(llvmName)) {
        if (existing->getType() != type) {
            reportGlobalConflict(unit, llvmName, existing->getType(), type);
        }
        return existing;
    }

    auto *llvmGlobal = scope.module.getGlobalVariable(llvmName);
    auto *llvmType = typeMgr->getLLVMType(type);
    if (llvmGlobal == nullptr) {
        llvmGlobal = new llvm::GlobalVariable(
            scope.module, llvmType, !isFullyWritableValueType(type),
            llvm::GlobalValue::ExternalLinkage, nullptr, llvmName);
    } else if (llvmGlobal->getValueType() != llvmType) {
        reportGlobalConflict(unit, llvmName, nullptr, type);
    }

    auto *obj = type->newObj(Object::VARIABLE);
    obj->setllvmValue(llvmGlobal);
    scope.addObj(llvmName, obj);
    return obj;
}

void
materializeStructMethodBindings(TypeTable *typeMgr, StructType *structType) {
    if (!typeMgr || !structType) {
        return;
    }

    for (const auto &method : structType->getMethodTypes()) {
        auto *storedMethodType = typeMgr->internType(method.second);
        auto *methodType =
            storedMethodType ? storedMethodType->as<FuncType>() : nullptr;
        if (!methodType) {
            continue;
        }
        if (typeMgr->getMethodFunction(structType, method.first())) {
            continue;
        }
        auto methodName = declarationsupport_impl::resolveStructMethodSymbolName(
            structType, method.first());
        auto *llvmFunc = llvm::Function::Create(
            getFunctionAbiLLVMType(*typeMgr, methodType, true),
            llvm::Function::ExternalLinkage, llvm::Twine(methodName),
            typeMgr->getModule());
        annotateFunctionAbi(*llvmFunc, methodType->getAbiKind());
        std::vector<string> paramNames;
        if (const auto *storedParamNames =
                structType->getMethodParamNames(method.first())) {
            paramNames = *storedParamNames;
        }
        typeMgr->bindMethodFunction(
            structType, method.first(),
            new Function(llvmFunc, methodType, std::move(paramNames), true));
    }
}

void
materializeStructTraitMethodBindings(TypeTable *typeMgr,
                                     StructType *structType) {
    if (!typeMgr || !structType) {
        return;
    }

    for (const auto &entry : structType->getTraitMethodTypes()) {
        const auto methodKey =
            traitMethodSlotKey(entry.second.traitName, entry.second.methodName);
        if (typeMgr->getMethodFunction(structType, toStringRef(methodKey))) {
            continue;
        }

        auto *storedMethodType = typeMgr->internType(entry.second.funcType);
        auto *methodType =
            storedMethodType ? storedMethodType->as<FuncType>() : nullptr;
        if (!methodType) {
            continue;
        }

        auto llvmName = declarationsupport_impl::resolveTraitMethodSymbolName(
            structType, toStringRef(entry.second.traitName),
            toStringRef(entry.second.methodName));
        auto *llvmFunc = typeMgr->getModule().getFunction(llvmName);
        if (!llvmFunc) {
            llvmFunc = llvm::Function::Create(
                getFunctionAbiLLVMType(*typeMgr, methodType, true),
                llvm::Function::ExternalLinkage, llvm::Twine(llvmName),
                typeMgr->getModule());
            annotateFunctionAbi(*llvmFunc, methodType->getAbiKind());
        }

        typeMgr->bindMethodFunction(
            structType, toStringRef(methodKey),
            new Function(llvmFunc, methodType, entry.second.paramNames, true));
    }
}

TypeClass *
materializeTraitMethodTypeByNode(TypeTable *typeMgr, CompilationUnit &unit,
                                 TypeNode *typeNode) {
    if (!typeMgr || !typeNode) {
        return nullptr;
    }
    return unit.resolveType(typeMgr, typeNode);
}

void
materializeConcreteTraitImplBodyMethods(TypeTable *typeMgr,
                                        CompilationUnit &unit,
                                        const ModuleInterface::TraitImplDecl &implDecl) {
    if (!typeMgr || !implDecl.hasBody || implDecl.isGeneric() ||
        implDecl.bodyMethods.empty() || !implDecl.selfTypeNode) {
        return;
    }

    auto *selfType = unit.resolveType(typeMgr, implDecl.selfTypeNode);
    auto *structType = selfType ? selfType->as<StructType>() : nullptr;
    if (!structType) {
        return;
    }

    for (const auto &method : implDecl.bodyMethods) {
        auto methodKey = traitMethodSlotKey(implDecl.traitName, method.localName);
        if (!structType->getTraitMethodTypeByKey(toStringRef(methodKey))) {
            std::vector<TypeClass *> argTypes;
            argTypes.reserve(method.paramTypeNodes.size() + 1);
            auto *selfPointee =
                method.receiverAccess == AccessKind::GetSet
                    ? static_cast<TypeClass *>(structType)
                    : static_cast<TypeClass *>(typeMgr->createConstType(structType));
            argTypes.push_back(typeMgr->createPointerType(selfPointee));
            for (std::size_t i = 0; i < method.paramTypeNodes.size(); ++i) {
                auto *paramType =
                    materializeTraitMethodTypeByNode(typeMgr, unit,
                                                    method.paramTypeNodes[i]);
                if (!paramType) {
                    internalError(
                        "failed to materialize concrete trait impl method `" +
                            toStdString(method.localName) +
                            "` parameter type `" +
                            describeTypeNode(
                                method.paramTypeNodes[i], "<unknown type>") + "`",
                        "Trait impl body interfaces should only store fully "
                        "resolved concrete type nodes.");
                }
                argTypes.push_back(paramType);
            }
            auto *retType =
                materializeTraitMethodTypeByNode(typeMgr, unit,
                                                method.returnTypeNode);
            auto paramBindingKinds = method.paramBindingKinds;
            paramBindingKinds.insert(paramBindingKinds.begin(),
                                     BindingKind::Value);
            auto *funcType = typeMgr->getOrCreateFunctionType(
                argTypes, retType, paramBindingKinds, AbiKind::Native);
            structType->addTraitMethodType(toStringRef(implDecl.traitName),
                                           toStringRef(method.localName),
                                           funcType, method.paramNames);
        }
    }

    materializeStructTraitMethodBindings(typeMgr, structType);
}

void
materializeReachableMethodBindings(
    TypeTable *typeMgr, TypeClass *type,
    std::unordered_set<const TypeClass *> &visitedTypes) {
    if (!typeMgr || !type || !visitedTypes.insert(type).second) {
        return;
    }

    if (auto *qualified = type->as<ConstType>()) {
        materializeReachableMethodBindings(typeMgr, qualified->getBaseType(),
                                           visitedTypes);
        return;
    }
    if (auto *pointer = type->as<PointerType>()) {
        materializeReachableMethodBindings(typeMgr, pointer->getPointeeType(),
                                           visitedTypes);
        return;
    }
    if (auto *indexable = type->as<IndexablePointerType>()) {
        materializeReachableMethodBindings(typeMgr, indexable->getElementType(),
                                           visitedTypes);
        return;
    }
    if (auto *array = type->as<ArrayType>()) {
        materializeReachableMethodBindings(typeMgr, array->getElementType(),
                                           visitedTypes);
        return;
    }
    if (auto *tuple = type->as<TupleType>()) {
        for (auto *itemType : tuple->getItemTypes()) {
            materializeReachableMethodBindings(typeMgr, itemType,
                                               visitedTypes);
        }
        return;
    }
    if (auto *funcType = type->as<FuncType>()) {
        for (auto *argType : funcType->getArgTypes()) {
            materializeReachableMethodBindings(typeMgr, argType, visitedTypes);
        }
        materializeReachableMethodBindings(typeMgr, funcType->getRetType(),
                                           visitedTypes);
        return;
    }
    if (auto *structType = type->as<StructType>()) {
        materializeStructMethodBindings(typeMgr, structType);
        materializeStructTraitMethodBindings(typeMgr, structType);
        for (const auto &member : structType->getMembers()) {
            materializeReachableMethodBindings(typeMgr, member.second.first,
                                               visitedTypes);
        }
        for (const auto &method : structType->getMethodTypes()) {
            materializeReachableMethodBindings(typeMgr, method.second,
                                               visitedTypes);
        }
        for (const auto &method : structType->getTraitMethodTypes()) {
            materializeReachableMethodBindings(typeMgr, method.second.funcType,
                                               visitedTypes);
        }
    }
}

void
materializeUnitInterface(Scope *global, CompilationUnit &unit,
                         bool exportNamespace, bool declareNamespace) {
    initBuildinType(global);
    ensureUnitInterfaceCollected(unit);
    auto *interface = unit.interface();
    auto *typeMgr = requireTypeTable(global);
    unit.clearLocalBindings();

    if (declareNamespace) {
        declareModuleNamespace(*global, unit);
    }

    std::unordered_set<const TypeClass *> reachableMethodTypes;

    for (const auto &entry : interface->types()) {
        auto *type = typeMgr->internType(entry.second.type);
        if (!type) {
            internalError(
                "failed to materialize imported type `" +
                    toStdString(entry.first) + "` from module `" +
                    toStdString(unit.path()) + "`",
                "Imported interfaces should only contain types that were "
                "successfully collected from the defining module.");
        }
        unit.bindLocalType(entry.first, toStdString(type->full_name));
        materializeReachableMethodBindings(typeMgr, type, reachableMethodTypes);
    }

    for (const auto &entry : interface->traits()) {
        unit.bindLocalTrait(entry.first, entry.second.exportedName);
    }

    for (const auto &entry : interface->functions()) {
        auto runtimeName =
            chooseFunctionRuntimeName(*global, entry.second, exportNamespace);
        unit.bindLocalFunction(entry.first, runtimeName);
        if (entry.second.isGeneric()) {
            continue;
        }
        auto *storedType = typeMgr->internType(entry.second.type);
        auto *funcType = storedType ? storedType->as<FuncType>() : nullptr;
        if (!funcType) {
            internalError(
                "failed to materialize imported function signature `" +
                    toStdString(entry.first) + "` from module `" +
                    toStdString(unit.path()) + "`",
                "Imported interfaces should only contain function signatures "
                "that were successfully collected from the defining module.");
        }
        materializeDeclaredFunction(*global, typeMgr, funcType,
                                    toStringRef(runtimeName),
                                    entry.second.paramNames, false, &unit);
        materializeReachableMethodBindings(typeMgr, storedType,
                                           reachableMethodTypes);
    }

    for (const auto &entry : interface->globals()) {
        auto *storedType = typeMgr->internType(entry.second.type);
        if (!storedType) {
            internalError("failed to materialize imported global `" +
                              toStdString(entry.first) + "` from module `" +
                              toStdString(unit.path()) + "`",
                          "Imported interfaces should only contain globals "
                          "with fully resolved types.");
        }
        auto runtimeName =
            exportNamespace ? entry.second.symbolName : entry.first;
        unit.bindLocalGlobal(entry.first, runtimeName);
        materializeDeclaredGlobal(*global, typeMgr, storedType,
                                  toStringRef(runtimeName), &unit);
        materializeReachableMethodBindings(typeMgr, storedType,
                                           reachableMethodTypes);
    }

    for (const auto &implDecl : interface->traitImpls()) {
        materializeConcreteTraitImplBodyMethods(typeMgr, unit, implDecl);
        auto *selfType = (!implDecl.isGeneric() && implDecl.selfTypeNode)
                             ? unit.resolveType(typeMgr, implDecl.selfTypeNode)
                             : typeMgr->getType(toStringRef(implDecl.selfTypeSpelling));
        materializeReachableMethodBindings(typeMgr, selfType,
                                           reachableMethodTypes);
    }

    unit.markInterfaceCollected();
}

}  // namespace moduleinterface_impl

void
collectUnitDeclarations(Scope *global, CompilationUnit &unit,
                        bool exportNamespace, bool declareNamespace) {
    moduleinterface_impl::materializeUnitInterface(global, unit,
                                                   exportNamespace,
                                                   declareNamespace);
}

}  // namespace lona
