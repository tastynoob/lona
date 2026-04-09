#include "lona/analyze/function.hh"
#include "lona/abi/abi.hh"
#include "lona/analyze/rules.hh"
#include "lona/ast/array_dim.hh"
#include "lona/ast/astnode.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/declare/support.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/calls.hh"
#include "lona/sema/hir.hh"
#include "lona/sema/initializer.hh"
#include "lona/sema/injectedmember.hh"
#include "lona/sema/moduleentry.hh"
#include "lona/sema/operatorresolver.hh"
#include "lona/sym/func.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include "parser.hh"
#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lona {
namespace analysis_impl {

namespace {

TypeTable *
requireTypeTable(Scope *scope) {
    assert(scope);
    auto *typeMgr = scope->types();
    assert(typeMgr);
    return typeMgr;
}

FuncType *
getOrCreateModuleEntryType(TypeTable *typeMgr) {
    return typeMgr->getOrCreateFunctionType({}, i32Ty);
}

llvm::Function *
getOrCreateModuleEntry(GlobalScope *global, TypeTable *typeMgr,
                       const CompilationUnit *unit, bool languageEntry) {
    auto *entryType = getOrCreateModuleEntryType(typeMgr);
    auto entryName = languageEntry ? languageEntrySymbolName().str()
                     : unit        ? moduleInitEntrySymbolName(*unit)
                                   : std::string();
    if (entryName.empty()) {
        throw DiagnosticError(
            DiagnosticError::Category::Internal,
            "module init entry is missing its symbol name",
            "Synthetic module entry requires a compilation unit.");
    }
    if (auto *existing = global->module.getFunction(entryName)) {
        return existing;
    }

    auto *entry =
        llvm::Function::Create(typeMgr->getLLVMFunctionType(entryType),
                               llvm::Function::ExternalLinkage,
                               llvm::Twine(entryName), global->module);
    annotateFunctionAbi(*entry, AbiKind::Native);
    return entry;
}

struct GenericRuntimeState {
    std::unordered_set<GenericInstanceKey, GenericInstanceKeyHash>
        inProgressInstances;
    std::unordered_set<GenericInstanceKey, GenericInstanceKeyHash>
        emittedInstances;
};

GenericRuntimeState &
genericRuntimeStateFor(HIRModule *module) {
    static std::unordered_map<HIRModule *, GenericRuntimeState> states;
    return states[module];
}

class GenericFunctionEmissionGuard {
    GenericRuntimeState &state_;
    GenericInstanceKey instanceKey_;
    bool completed_ = false;

public:
    GenericFunctionEmissionGuard(GenericRuntimeState &state,
                                 GenericInstanceKey instanceKey)
        : state_(state), instanceKey_(std::move(instanceKey)) {
        state_.inProgressInstances.insert(instanceKey_);
    }

    ~GenericFunctionEmissionGuard() {
        state_.inProgressInstances.erase(instanceKey_);
        if (completed_) {
            state_.emittedInstances.insert(instanceKey_);
        }
    }

    void markCompleted() { completed_ = true; }
};

std::string
displayTraitReceiverSegment(llvm::StringRef resolvedTraitName) {
    auto dotPos = resolvedTraitName.rfind('.');
    if (dotPos == llvm::StringRef::npos) {
        return resolvedTraitName.str();
    }
    return resolvedTraitName.substr(dotPos + 1).str();
}

FuncType *
getStructMethodTypeByKey(StructType *structType, llvm::StringRef methodKey) {
    if (!structType) {
        return nullptr;
    }
    if (auto *methodType = structType->getMethodType(methodKey)) {
        return methodType;
    }
    return structType->getTraitMethodTypeByKey(methodKey);
}

const std::vector<string> *
getStructMethodParamNamesByKey(const StructType *structType,
                               llvm::StringRef methodKey) {
    if (!structType) {
        return nullptr;
    }
    if (auto *paramNames = structType->getMethodParamNames(methodKey)) {
        return paramNames;
    }
    return structType->getTraitMethodParamNamesByKey(methodKey);
}

}  // namespace

class FunctionAnalyzer {
    TypeTable *typeMgr;
    GlobalScope *global;
    const CompilationUnit *unit;
    const ResolvedFunction &resolved;
    OperatorResolver operatorResolver;
    HIRModule *ownerModule;
    HIRFunc *hirFunc;
    AnalysisLookupCache localLookupCache_;
    AnalysisLookupCache *lookupCache;
    std::unordered_map<const ResolvedLocalBinding *, ObjectPtr> bindingObjects;
    int loopDepth = 0;

    [[noreturn]] void error(const location &loc, const std::string &message,
                            const std::string &hint = std::string()) {
        lona::error(loc, message, hint);
    }

    [[noreturn]] void internalError(const location &loc,
                                    const std::string &message,
                                    const std::string &hint = std::string()) {
        throw DiagnosticError(DiagnosticError::Category::Internal, loc, message,
                              hint);
    }

    std::string currentFunctionDisplayName() const {
        if (resolved.decl()) {
            return toStdString(resolved.decl()->name);
        }
        if (resolved.hasDeclaredFunction()) {
            return toStdString(resolved.functionName());
        }
        return "<function>";
    }

    TypeClass *requireType(TypeNode *node, const location &loc,
                           const std::string &context) {
        validateTypeNodeLayout(node);
        if (isReservedInitialListTypeNode(node)) {
            errorReservedInitialListType(node->loc);
        }
        TypeClass *type = nullptr;
        if (!resolved.concreteGenericTypes().empty()) {
            type = substituteGenericSignatureType(
                node, resolved.concreteGenericTypes(), loc,
                currentFunctionDisplayName(),
                resolved.genericOwnerInterface());
        } else {
            type = unit ? unit->resolveType(typeMgr, node)
                        : typeMgr->getType(node);
        }
        if (!type) {
            error(loc, context);
        }
        return type;
    }

    void requireCompatibleTypes(const location &loc, TypeClass *expectedType,
                                TypeClass *actualType,
                                const std::string &context) {
        requireCompatibleInitializerTypes(loc, expectedType, actualType,
                                          context);
    }

    bool canBindReferenceType(TypeClass *targetType, TypeClass *sourceType) {
        return targetType && sourceType &&
               isConstQualificationConvertible(targetType, sourceType);
    }

    [[noreturn]] void errorReadOnlyAssignmentTarget(const location &loc,
                                                    TypeClass *type) {
        error(loc,
              "assignment target contains read-only storage: " +
                  describeResolvedType(type),
              "Only fully writable values can appear on the left side of `=`. "
              "Write through a mutable projection instead, or copy into a new "
              "`var` binding if you need a writable whole value.");
    }

    template<typename T, typename... Args>
    T *makeHIR(Args &&...args) {
        assert(ownerModule);
        return ownerModule->create<T>(std::forward<Args>(args)...);
    }

    void bindObject(const ResolvedLocalBinding *binding, ObjectPtr object) {
        assert(binding);
        assert(object);
        bindingObjects[binding] = object;
    }

    AstNode *makeStaticDimensionNode(std::size_t extent, const location &loc) {
        auto text = std::to_string(extent);
        AstToken token(TokenType::ConstInt32, text.c_str(), loc);
        return new AstConst(token);
    }

    ObjectPtr requireBoundObject(const ResolvedLocalBinding *binding,
                                 const location &loc) {
        if (!binding) {
            internalError(loc, "missing resolved local binding",
                          "Run name resolution before HIR lowering.");
        }
        auto found = bindingObjects.find(binding);
        if (found == bindingObjects.end()) {
            internalError(loc,
                          "resolved local binding `" +
                              toStdString(binding->name()) +
                              "` was not materialized before use",
                          "This looks like a compiler pipeline bug.");
        }
        return found->second;
    }

    ObjectPtr requireGlobalObject(const ::string &name, const location &loc,
                                  const std::string &context) {
        auto *obj = global->getObj(name);
        if (!obj) {
            internalError(
                loc,
                "resolved " + context + " `" + toStdString(name) +
                    "` is missing from the current global scope",
                "Rebuild declarations before reusing this resolved module.");
        }
        return obj;
    }

    Function *requireGlobalFunction(const ::string &name, const location &loc,
                                    const std::string &context) {
        auto *obj = requireGlobalObject(name, loc, context);
        auto *func = obj->as<Function>();
        if (!func) {
            internalError(
                loc,
                "resolved " + context + " `" + toStdString(name) +
                    "` no longer refers to a function",
                "Rebuild declarations before reusing this resolved module.");
        }
        return func;
    }

    StructType *requireStructTypeByName(const ::string &name,
                                        const location &loc,
                                        const std::string &context) {
        auto *type = typeMgr->getType(name);
        auto *structType = type ? type->as<StructType>() : nullptr;
        if (!structType) {
            internalError(
                loc,
                "resolved " + context + " `" + toStdString(name) +
                    "` is missing from the current type table",
                "Rebuild declarations before reusing this resolved module.");
        }
        return structType;
    }

    TypeClass *requireTypeByName(const ::string &name, const location &loc,
                                 const std::string &context) {
        auto *type = typeMgr->getType(name);
        if (!type) {
            internalError(
                loc,
                "resolved " + context + " `" + toStdString(name) +
                    "` is missing from the current type table",
                "Rebuild declarations before reusing this resolved module.");
        }
        return type;
    }

    bool isLocalGenericTemplateOwner(
        const ModuleInterface::FunctionDecl *functionDecl,
        const ModuleInterface *ownerInterface) const {
        if (!functionDecl || !unit) {
            return false;
        }
        if (ownerInterface && unit->interface() &&
            ownerInterface != unit->interface()) {
            return false;
        }
        auto lookup = unit->lookupTopLevelName(functionDecl->localName);
        return lookup.isFunction() && lookup.functionDecl == functionDecl;
    }

    const CompilationUnit *templateOwnerUnit(
        const ModuleInterface *ownerInterface) const {
        if (!unit) {
            return nullptr;
        }
        return unit->contextUnitForInterface(ownerInterface);
    }

    bool isLocalGenericTemplateOwner(
        const ModuleInterface::TypeDecl *typeDecl,
        const ModuleInterface *ownerInterface) const {
        auto *ownerUnit = templateOwnerUnit(ownerInterface);
        return ownerUnit == unit && ownerUnit && ownerUnit->ownsTypeDecl(typeDecl);
    }

    const AstFuncDecl *findGenericFunctionDecl(
        const CompilationUnit *templateUnit,
        const ModuleInterface::FunctionDecl &functionDecl) const {
        if (!templateUnit) {
            return nullptr;
        }
        auto *root = templateUnit->syntaxTree();
        auto *program = dynamic_cast<AstProgram *>(root);
        auto *body =
            dynamic_cast<AstStatList *>(program ? program->body : root);
        if (!body) {
            return nullptr;
        }
        for (auto *stmt : body->getBody()) {
            auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt);
            if (!funcDecl || !funcDecl->hasTypeParams()) {
                continue;
            }
            if (toStdString(funcDecl->name) ==
                toStdString(functionDecl.localName)) {
                return funcDecl;
            }
        }
        return nullptr;
    }

    const AstStructDecl *findStructDecl(
        const CompilationUnit *templateUnit,
        const ModuleInterface::TypeDecl &typeDecl) const {
        if (!templateUnit) {
            return nullptr;
        }
        ensureStructSyntaxIndex(templateUnit);
        auto foundUnit = lookupCache->structSyntaxByUnit.find(templateUnit);
        if (foundUnit == lookupCache->structSyntaxByUnit.end()) {
            return nullptr;
        }
        auto found = foundUnit->second.find(typeDecl.localName);
        return found != foundUnit->second.end() ? found->second : nullptr;
    }

    const CompilationUnit *traitImplOwnerUnit(
        const CompilationUnit::VisibleTraitImpl &visibleImpl) const {
        if (visibleImpl.importedModule) {
            return visibleImpl.importedModule->unit;
        }
        return unit;
    }

    const ModuleInterface *traitImplOwnerInterface(
        const CompilationUnit::VisibleTraitImpl &visibleImpl) const {
        auto *ownerUnit = traitImplOwnerUnit(visibleImpl);
        if (!ownerUnit || ownerUnit == unit) {
            return nullptr;
        }
        return ownerUnit->interface();
    }

    std::string traitImplTemplateName(
        const ModuleInterface::TraitImplDecl &implDecl) const {
        return toStdString(implDecl.traitName) + " for " +
               toStdString(implDecl.selfTypeSpelling);
    }

    const ModuleInterface::MethodTemplateDecl *findTraitImplBodyMethodTemplate(
        const ModuleInterface::TraitImplDecl &implDecl,
        llvm::StringRef methodName) const {
        auto localMethodName = methodName.str();
        auto prefix = traitMethodSlotKey(implDecl.traitName, string());
        if (!prefix.empty() && localMethodName.rfind(prefix, 0) == 0) {
            localMethodName.erase(0, prefix.size());
        }
        for (const auto &method : implDecl.bodyMethods) {
            if (toStdString(method.localName) == localMethodName) {
                return &method;
            }
        }
        return nullptr;
    }

    const ModuleInterface::TypeDecl *findVisibleTypeDeclForStructType(
        StructType *structType, const CompilationUnit **ownerUnitOut) const {
        if (ownerUnitOut) {
            *ownerUnitOut = nullptr;
        }
        if (!structType || !unit) {
            return nullptr;
        }
        if (auto *appliedTemplateDecl =
                findAppliedTemplateDecl(structType, ownerUnitOut)) {
            return appliedTemplateDecl;
        }
        auto lookup = visibleTypeLookupFor(structType);
        if (!lookup.found()) {
            return nullptr;
        }
        if (ownerUnitOut) {
            *ownerUnitOut = lookup.ownerUnit;
        }
        return lookup.typeDecl;
    }

    const ModuleInterface::MethodTemplateDecl *findGenericMethodTemplateDecl(
        const ModuleInterface::TypeDecl &typeDecl,
        llvm::StringRef methodName) const {
        for (const auto &method : typeDecl.methodTemplates) {
            if (toStringRef(method.localName) == methodName &&
                method.typeParams.size() > method.enclosingTypeParamCount) {
                return &method;
            }
        }
        return nullptr;
    }

    const AstFuncDecl *findLocalStructMethodDecl(const CompilationUnit *ownerUnit,
                                                 const AstStructDecl *structDecl,
                                                 llvm::StringRef methodName) const {
        if (!ownerUnit || !structDecl) {
            return nullptr;
        }
        ensureStructSyntaxIndex(ownerUnit);
        auto foundStruct = lookupCache->methodSyntaxByStruct.find(structDecl);
        if (foundStruct == lookupCache->methodSyntaxByStruct.end()) {
            return nullptr;
        }
        auto foundMethod = foundStruct->second.find(string(methodName.str()));
        return foundMethod != foundStruct->second.end() ? foundMethod->second
                                                        : nullptr;
    }

    const ModuleInterface::TypeDecl *findAppliedTemplateDecl(
        StructType *structType, const CompilationUnit **templateUnitOut) const {
        if (templateUnitOut) {
            *templateUnitOut = nullptr;
        }
        if (!structType || !structType->isAppliedTemplateInstance()) {
            return nullptr;
        }
        auto *templateUnit = structType->getAppliedTemplateOwnerUnit();
        if (!templateUnit) {
            templateUnit = unit;
        }
        if (!templateUnit || !templateUnit->interface()) {
            return nullptr;
        }
        if (templateUnitOut) {
            *templateUnitOut = templateUnit;
        }
        return typeDeclByExportedName(templateUnit,
                                      structType->getAppliedTemplateName());
    }

    void ensureVisibleTypeLookup() const {
        if (lookupCache->visibleTypesReady) {
            return;
        }
        lookupCache->visibleTypesReady = true;
        auto *rootUnit = lookupCache->rootUnit ? lookupCache->rootUnit : unit;
        if (!rootUnit) {
            return;
        }
        indexVisibleTypesFrom(rootUnit);
        for (const auto &imported : rootUnit->importedModules()) {
            if (!imported.second.interface || !imported.second.unit) {
                continue;
            }
            indexVisibleTypesFrom(imported.second.unit);
        }
    }

    void indexVisibleTypesFrom(const CompilationUnit *ownerUnit) const {
        if (!ownerUnit) {
            return;
        }
        ensureTypeDeclExportedIndex(ownerUnit);
        auto foundUnit = lookupCache->typeDeclsByExportedName.find(ownerUnit);
        if (foundUnit == lookupCache->typeDeclsByExportedName.end()) {
            return;
        }
        for (const auto &entry : foundUnit->second) {
            AnalysisLookupCache::VisibleTypeLookup lookup{entry.second, ownerUnit};
            lookupCache->visibleTypesByExportedName.emplace(entry.first, lookup);
            if (ownerUnit == unit) {
                if (auto *declStructType =
                        asUnqualified<StructType>(entry.second->type)) {
                    lookupCache->visibleTypesByRuntimeType.emplace(declStructType,
                                                                   lookup);
                }
            }
        }
    }

    void ensureTypeDeclExportedIndex(const CompilationUnit *ownerUnit) const {
        if (!ownerUnit ||
            lookupCache->typeDeclsByExportedName.count(ownerUnit) != 0) {
            return;
        }
        std::unordered_map<string, const ModuleInterface::TypeDecl *> indexed;
        if (auto *ownerInterface = ownerUnit->interface()) {
            indexed.reserve(ownerInterface->types().size());
            for (const auto &entry : ownerInterface->types()) {
                indexed.emplace(entry.second.exportedName, &entry.second);
            }
        }
        lookupCache->typeDeclsByExportedName.emplace(ownerUnit,
                                                     std::move(indexed));
    }

    void ensureStructSyntaxIndex(const CompilationUnit *ownerUnit) const {
        if (!ownerUnit ||
            lookupCache->structSyntaxByUnit.count(ownerUnit) != 0) {
            return;
        }
        std::unordered_map<string, const AstStructDecl *> structsByLocalName;
        auto *root = ownerUnit->syntaxTree();
        auto *program = dynamic_cast<AstProgram *>(root);
        auto *body =
            dynamic_cast<AstStatList *>(program ? program->body : root);
        if (body) {
            structsByLocalName.reserve(body->getBody().size());
            for (auto *stmt : body->getBody()) {
                auto *structDecl = dynamic_cast<AstStructDecl *>(stmt);
                if (!structDecl) {
                    continue;
                }
                structsByLocalName.emplace(structDecl->name, structDecl);
                auto *structBody = dynamic_cast<AstStatList *>(structDecl->body);
                if (!structBody) {
                    continue;
                }
                auto &methods = lookupCache->methodSyntaxByStruct[structDecl];
                methods.reserve(structBody->getBody().size());
                for (auto *member : structBody->getBody()) {
                    auto *funcDecl = dynamic_cast<AstFuncDecl *>(member);
                    if (!funcDecl) {
                        continue;
                    }
                    methods.emplace(funcDecl->name, funcDecl);
                }
            }
        }
        lookupCache->structSyntaxByUnit.emplace(ownerUnit,
                                                std::move(structsByLocalName));
    }

    const ModuleInterface::TypeDecl *
    typeDeclByExportedName(const CompilationUnit *ownerUnit,
                           const string &exportedName) const {
        ensureTypeDeclExportedIndex(ownerUnit);
        auto foundUnit = lookupCache->typeDeclsByExportedName.find(ownerUnit);
        if (foundUnit == lookupCache->typeDeclsByExportedName.end()) {
            return nullptr;
        }
        auto foundType = foundUnit->second.find(exportedName);
        return foundType != foundUnit->second.end() ? foundType->second
                                                    : nullptr;
    }

    AnalysisLookupCache::VisibleTypeLookup
    visibleTypeLookupFor(StructType *structType) const {
        ensureVisibleTypeLookup();
        auto foundByType = lookupCache->visibleTypesByRuntimeType.find(structType);
        if (foundByType != lookupCache->visibleTypesByRuntimeType.end()) {
            return foundByType->second;
        }
        return visibleTypeLookupByExportedName(structType->full_name);
    }

    AnalysisLookupCache::VisibleTypeLookup
    visibleTypeLookupByExportedName(const string &exportedName) const {
        ensureVisibleTypeLookup();
        auto found = lookupCache->visibleTypesByExportedName.find(exportedName);
        return found != lookupCache->visibleTypesByExportedName.end()
                   ? found->second
                   : AnalysisLookupCache::VisibleTypeLookup{};
    }

    StructType *instantiateGenericStructType(
        const ModuleInterface::TypeDecl &typeDecl,
        const std::vector<TypeClass *> &genericArgs, const location &loc,
        const ModuleInterface *ownerInterface = nullptr) {
        if (!unit) {
            return nullptr;
        }
        auto *contextUnit = templateOwnerUnit(ownerInterface);
        if (!contextUnit) {
            return nullptr;
        }
        auto *structType =
            unit->materializeAppliedStructType(typeMgr, typeDecl,
                                               std::vector<TypeClass *>(
                                                   genericArgs),
                                               *contextUnit);
        if (!structType) {
            internalError(
                loc,
                "generic struct `" +
                    toStdString(typeDecl.localName) +
                    "` did not materialize a concrete runtime type",
                "This looks like a generic struct instantiation bug.");
        }
        return structType;
    }

    std::unordered_map<std::string, TypeClass *> buildAppliedStructGenericArgs(
        const ModuleInterface::TypeDecl &typeDecl, StructType *structType,
        const location &loc) {
        if (!structType) {
            return {};
        }
        const auto &appliedTypeArgs = structType->getAppliedTypeArgs();
        if (typeDecl.typeParams.size() != appliedTypeArgs.size()) {
            internalError(
                loc,
                "generic struct instance `" +
                    describeResolvedType(structType) +
                    "` is missing concrete type arguments for its template",
                "This looks like a generic struct instantiation bug.");
        }
        std::unordered_map<std::string, TypeClass *> genericArgs;
        genericArgs.reserve(typeDecl.typeParams.size());
        for (std::size_t i = 0; i < typeDecl.typeParams.size(); ++i) {
            genericArgs.emplace(toStdString(typeDecl.typeParams[i].localName),
                                appliedTypeArgs[i]);
        }
        return genericArgs;
    }

    std::vector<string> buildConcreteTypeArgNames(
        const std::vector<TypeClass *> &typeArgs, const location &loc,
        const std::string &context) {
        std::vector<string> names;
        names.reserve(typeArgs.size());
        for (std::size_t i = 0; i < typeArgs.size(); ++i) {
            if (!typeArgs[i]) {
                internalError(
                    loc,
                    context + " is missing a concrete type argument at index " +
                        std::to_string(i),
                    "This looks like a generic instantiation bug.");
            }
            names.push_back(typeArgs[i]->full_name);
        }
        return names;
    }

    std::vector<string> buildConcreteTypeArgNames(
        const std::vector<ModuleInterface::GenericParamDecl> &typeParams,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const location &loc, const std::string &context) {
        std::vector<string> names;
        names.reserve(typeParams.size());
        for (const auto &param : typeParams) {
            auto paramName = toStdString(param.localName);
            auto found = genericArgs.find(paramName);
            if (found == genericArgs.end() || !found->second) {
                internalError(
                    loc,
                    context + " is missing a concrete type for `" + paramName +
                        "`",
                    "This looks like a generic argument selection bug.");
            }
            names.push_back(found->second->full_name);
        }
        return names;
    }

    GenericTemplateRevision buildTemplateRevision(
        const CompilationUnit *ownerUnit) const {
        if (!ownerUnit) {
            return {};
        }
        return {ownerUnit->interfaceHash(), ownerUnit->implementationHash(),
                ownerUnit->visibleImportInterfaceHash(),
                unit ? unit->visibleTraitImplHash() : 0};
    }

    GenericInstanceRegistry *genericInstanceRegistry() const {
        return global ? global->genericInstanceRegistry() : nullptr;
    }

    bool claimGenericInstanceEmission(const GenericInstanceKey &key) const {
        if (!unit) {
            return true;
        }
        auto *registry = genericInstanceRegistry();
        if (!registry) {
            return true;
        }
        if (const auto *emitter = registry->emitterModuleKey(key)) {
            return *emitter == unit->moduleKey();
        }
        registry->claim(key, unit->moduleKey());
        return true;
    }

    GenericInstanceKey buildFunctionInstanceKey(
        const ModuleInterface::FunctionDecl &functionDecl,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const location &loc, const ModuleInterface *ownerInterface) {
        auto *ownerUnit = templateOwnerUnit(ownerInterface);
        if (!ownerUnit || !unit) {
            internalError(loc,
                          "generic function instance is missing its requester "
                          "or owner module",
                          "This looks like a generic instantiation bug.");
        }
        GenericInstanceKey key;
        key.requesterModuleKey = unit->moduleKey();
        key.ownerModuleKey = ownerUnit->moduleKey();
        key.kind = GenericInstanceKind::Function;
        key.templateName = functionDecl.localName;
        key.concreteTypeArgs = buildConcreteTypeArgNames(
            functionDecl.typeParams, genericArgs, loc,
            "generic function `" + toStdString(functionDecl.localName) +
                "` instance");
        return key;
    }

    GenericInstanceKey buildStructMethodInstanceKey(
        const ModuleInterface::TypeDecl &typeDecl, StructType *structType,
        const CompilationUnit *templateUnit, llvm::StringRef methodName,
        const std::vector<TypeClass *> &methodTypeArgs, const location &loc) {
        if (!structType || !templateUnit || !unit) {
            internalError(loc,
                          "generic struct method instance is missing its "
                          "requester, owner, or concrete self type",
                          "This looks like a generic method instantiation bug.");
        }
        GenericInstanceKey key;
        key.requesterModuleKey = unit->moduleKey();
        key.ownerModuleKey = templateUnit->moduleKey();
        key.kind = GenericInstanceKind::Method;
        key.templateName = typeDecl.exportedName;
        key.methodName = string(methodName);
        key.concreteTypeArgs = buildConcreteTypeArgNames(
            structType->getAppliedTypeArgs(), loc,
            "generic struct method `" + methodName.str() + "` instance");
        auto methodArgNames = buildConcreteTypeArgNames(
            methodTypeArgs, loc,
            "generic struct method `" + methodName.str() + "` instance");
        key.concreteTypeArgs.insert(key.concreteTypeArgs.end(),
                                    methodArgNames.begin(),
                                    methodArgNames.end());
        return key;
    }

    GenericInstanceKey buildTraitImplMethodInstanceKey(
        const ModuleInterface::TraitImplDecl &implDecl, StructType *structType,
        const CompilationUnit *ownerUnit, llvm::StringRef methodName,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const location &loc) {
        if (!structType || !ownerUnit || !unit) {
            internalError(
                loc,
                "generic trait impl method instance is missing its requester, "
                "owner, or concrete self type",
                "This looks like a trait impl instantiation bug.");
        }
        GenericInstanceKey key;
        key.requesterModuleKey = unit->moduleKey();
        key.ownerModuleKey = ownerUnit->moduleKey();
        key.kind = GenericInstanceKind::Method;
        key.templateName = traitImplTemplateName(implDecl);
        key.methodName = string(methodName);
        key.concreteTypeArgs = buildConcreteTypeArgNames(
            implDecl.typeParams, genericArgs, loc,
            "generic trait impl method `" + methodName.str() + "` instance");
        return key;
    }

    std::unordered_map<std::string, TypeClass *> resolveTraitImplGenericArgs(
        const ModuleInterface::TraitImplDecl &implDecl, StructType *structType,
        const location &loc, const ModuleInterface *ownerInterface) {
        std::unordered_map<std::string, TypeClass *> genericArgs;
        genericArgs.reserve(implDecl.typeParams.size());
        for (const auto &param : implDecl.typeParams) {
            genericArgs.emplace(toStdString(param.localName), nullptr);
        }
        if (implDecl.typeParams.empty()) {
            return genericArgs;
        }
        if (!implDecl.selfTypeNode || !structType) {
            internalError(
                loc,
                "generic trait impl is missing its self type pattern or "
                "concrete receiver type",
                "This looks like a trait impl instantiation bug.");
        }
        inferGenericArgsFromPattern(
            implDecl.selfTypeNode, structType, genericArgs, loc,
            traitImplTemplateName(implDecl), ownerInterface);
        for (const auto &param : implDecl.typeParams) {
            auto paramName = toStdString(param.localName);
            auto found = genericArgs.find(paramName);
            if (found == genericArgs.end() || !found->second) {
                internalError(
                    loc,
                    "generic trait impl `" + traitImplTemplateName(implDecl) +
                        "` is missing a concrete type for `" + paramName + "`",
                    "This looks like a trait impl instantiation bug.");
            }
        }
        return genericArgs;
    }

    void recordGenericInstance(GenericInstanceKey key,
                               const CompilationUnit *ownerUnit,
                               std::vector<string> emittedSymbolNames) {
        if (!unit) {
            return;
        }
        unit->recordGenericInstance(
            {std::move(key), buildTemplateRevision(ownerUnit),
             std::move(emittedSymbolNames)});
    }

    std::string buildLocalGenericStructMethodInstanceSymbolName(
        StructType *structType, llvm::StringRef methodName,
        const std::vector<TypeClass *> &methodTypeArgs, const location &loc) {
        if (!structType) {
            internalError(loc,
                          "generic struct method is missing its concrete self "
                          "type",
                          "This looks like a method instantiation bug.");
        }
        auto symbolName =
            declarationsupport_impl::resolveStructMethodSymbolName(
                structType, methodName);
        if (methodTypeArgs.empty()) {
            return symbolName;
        }
        symbolName += "__inst";
        for (auto *typeArg : methodTypeArgs) {
            if (!typeArg) {
                internalError(
                    loc,
                    "generic struct method `" + methodName.str() +
                        "` instance is missing a concrete method type argument",
                    "This looks like a generic method instantiation bug.");
            }
            symbolName += "__" + mangleModuleEntryComponent(typeArg->full_name);
        }
        return symbolName;
    }

    Function *declareLocalGenericStructMethodInstance(
        StructType *structType, llvm::StringRef methodName,
        const location &loc) {
        if (auto *existing = typeMgr->getMethodFunction(structType, methodName)) {
            auto existingSymbolName =
                existing->getllvmValue()
                    ? llvm::cast<llvm::Function>(existing->getllvmValue())
                          ->getName()
                          .str()
                    : buildLocalGenericStructMethodInstanceSymbolName(
                          structType, methodName, {}, loc);
            if (!global->getObj(string(existingSymbolName))) {
                global->addObj(string(existingSymbolName), existing);
            }
            return existing;
        }
        auto symbolName =
            buildLocalGenericStructMethodInstanceSymbolName(structType,
                                                            methodName, {}, loc);
        if (auto *existingObj = global->getObj(string(symbolName))) {
            auto *func = existingObj->as<Function>();
            if (!func) {
                internalError(
                    loc,
                    "generic struct method instance symbol `" + symbolName +
                        "` collides with a non-function global",
                    "This looks like a symbol declaration bug.");
            }
            typeMgr->bindMethodFunction(structType, methodName, func);
            return func;
        }
        auto *funcType = structType->getMethodType(methodName);
        if (!funcType) {
            internalError(
                loc,
                "generic struct method `" + methodName.str() +
                    "` is missing its concrete signature on `" +
                    describeResolvedType(structType) + "`",
                "Materialize the concrete struct before lowering method "
                "calls.");
        }
        std::vector<string> paramNames;
        if (const auto *storedParamNames =
                structType->getMethodParamNames(methodName)) {
            paramNames = *storedParamNames;
        }
        auto *llvmFunc = global->module.getFunction(symbolName);
        if (!llvmFunc) {
            llvmFunc = llvm::Function::Create(
                getFunctionAbiLLVMType(*typeMgr, funcType, true),
                llvm::Function::ExternalLinkage, llvm::Twine(symbolName),
                global->module);
            annotateFunctionAbi(*llvmFunc, funcType->getAbiKind());
        }
        auto *func =
            new Function(llvmFunc, funcType, std::move(paramNames), true);
        typeMgr->bindMethodFunction(structType, methodName, func);
        global->addObj(string(symbolName), func);
        return func;
    }

    std::string buildLocalGenericFunctionInstanceSymbolName(
        const ModuleInterface::FunctionDecl &functionDecl,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const location &loc) {
        auto baseName = !toStdString(functionDecl.symbolName).empty()
                            ? toStdString(functionDecl.symbolName)
                            : toStdString(functionDecl.localName);
        std::string symbolName = baseName + "__inst";
        for (const auto &param : functionDecl.typeParams) {
            auto paramName = toStdString(param.localName);
            auto found = genericArgs.find(paramName);
            if (found == genericArgs.end() || !found->second) {
                internalError(
                    loc,
                    "generic function instance is missing a concrete type for `" +
                        paramName + "`",
                    "This looks like a generic argument selection bug.");
            }
            symbolName += "__" +
                          mangleModuleEntryComponent(found->second->full_name);
        }
        return symbolName;
    }

    Function *declareGenericFunctionInstance(
        const ModuleInterface::FunctionDecl &functionDecl,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const std::string &symbolName, const location &loc,
        const ModuleInterface *ownerInterface) {
        if (auto *existing = global->getObj(string(symbolName))) {
            auto *func = existing->as<Function>();
            if (!func) {
                internalError(
                    loc,
                    "generic function instance symbol `" + symbolName +
                        "` collides with a non-function global",
                    "This looks like a symbol declaration bug.");
            }
            return func;
        }

        std::vector<TypeClass *> argTypes;
        argTypes.reserve(functionDecl.paramTypeNodes.size());
        for (auto *paramTypeNode : functionDecl.paramTypeNodes) {
            argTypes.push_back(substituteGenericSignatureType(
                paramTypeNode, genericArgs, loc,
                toStdString(functionDecl.localName), ownerInterface));
        }
        auto *retType = substituteGenericSignatureType(
            functionDecl.returnTypeNode, genericArgs, loc,
            toStdString(functionDecl.localName), ownerInterface);
        auto *funcType = typeMgr->getOrCreateFunctionType(
            argTypes, retType, functionDecl.paramBindingKinds,
            functionDecl.abiKind);
        if (!funcType) {
            internalError(
                loc,
                "failed to build concrete function type for `" + symbolName +
                    "`",
                "This looks like a generic instantiation bug.");
        }

        auto *llvmFunc = llvm::Function::Create(
            getFunctionAbiLLVMType(*typeMgr, funcType, false),
            llvm::Function::ExternalLinkage, llvm::Twine(symbolName),
            global->module);
        annotateFunctionAbi(*llvmFunc, funcType->getAbiKind());
        auto *func =
            new Function(llvmFunc, funcType, functionDecl.paramNames, false);
        global->addObj(string(symbolName), func);
        return func;
    }

    HIRFunc *findOwnerModuleFunction(const std::string &symbolName) const {
        if (!ownerModule) {
            return nullptr;
        }
        for (auto *func : ownerModule->getFunctions()) {
            auto *llvmFunc = func ? func->getLLVMFunction() : nullptr;
            if (llvmFunc && llvmFunc->getName() == llvm::StringRef(symbolName)) {
                return func;
            }
        }
        return nullptr;
    }

    Function *instantiateGenericFunction(
        const ModuleInterface::FunctionDecl &functionDecl,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const location &loc, const ModuleInterface *ownerInterface) {
        auto *templateUnit = templateOwnerUnit(ownerInterface);
        auto *templateDecl = findGenericFunctionDecl(templateUnit, functionDecl);
        if (!templateDecl) {
            internalError(
                loc,
                "generic function `" +
                    toStdString(functionDecl.localName) +
                    "` is missing its template AST",
                "This looks like a generic template registration bug.");
        }

        auto symbolName =
            buildLocalGenericFunctionInstanceSymbolName(functionDecl,
                                                       genericArgs, loc);
        auto *func = declareGenericFunctionInstance(functionDecl, genericArgs,
                                                    symbolName, loc,
                                                    ownerInterface);
        auto instanceKey =
            buildFunctionInstanceKey(functionDecl, genericArgs, loc,
                                     ownerInterface);
        const bool shouldEmit =
            claimGenericInstanceEmission(instanceKey);
        recordGenericInstance(instanceKey, templateUnit,
                              shouldEmit ? std::vector<string>{string(symbolName)}
                                         : std::vector<string>{});
        if (!shouldEmit) {
            return func;
        }

        auto &runtimeState = genericRuntimeStateFor(ownerModule);
        if (runtimeState.emittedInstances.count(instanceKey) != 0 ||
            runtimeState.inProgressInstances.count(instanceKey) != 0 ||
            findOwnerModuleFunction(symbolName)) {
            return func;
        }

        GenericFunctionEmissionGuard guard(runtimeState, instanceKey);
        auto resolvedModule = resolveGenericFunctionInstance(
            global, templateUnit, templateDecl, symbolName,
            templateUnit != unit ? templateUnit->interface() : nullptr,
            genericArgs);
        if (!resolvedModule || resolvedModule->functions().size() != 1) {
            internalError(
                loc,
                "generic function instance `" + symbolName +
                    "` did not resolve to exactly one function body",
                "This looks like a generic resolve bug.");
        }

        auto *hirInstance = analyzeResolvedFunction(
            global, ownerModule, unit, *resolvedModule->functions().front(),
            lookupCache);
        if (!findOwnerModuleFunction(symbolName)) {
            ownerModule->addFunction(hirInstance);
        }
        guard.markCompleted();
        return func;
    }

    Function *instantiateGenericStructMethod(StructType *structType,
                                             llvm::StringRef methodName,
                                             const location &loc) {
        const CompilationUnit *templateUnit = nullptr;
        auto *typeDecl = findAppliedTemplateDecl(structType, &templateUnit);
        if (!typeDecl) {
            return typeMgr->getMethodFunction(structType, methodName);
        }

        auto *templateDecl = findStructDecl(templateUnit, *typeDecl);
        auto *methodDecl =
            findLocalStructMethodDecl(templateUnit, templateDecl, methodName);
        if (!templateDecl || !methodDecl) {
            internalError(
                loc,
                "generic struct method `" + methodName.str() +
                    "` is missing its template AST",
                "This looks like a generic struct method registration bug.");
        }

        auto *func =
            declareLocalGenericStructMethodInstance(structType, methodName, loc);
        auto symbolName = func->getllvmValue()
                              ? func->getllvmValue()->getName().str()
                              : buildLocalGenericStructMethodInstanceSymbolName(
                                    structType, methodName, {}, loc);
        auto instanceKey = buildStructMethodInstanceKey(
            *typeDecl, structType, templateUnit, methodName, {}, loc);
        const bool shouldEmit =
            claimGenericInstanceEmission(instanceKey);
        recordGenericInstance(instanceKey, templateUnit,
                              shouldEmit ? std::vector<string>{string(symbolName)}
                                         : std::vector<string>{});
        if (!shouldEmit) {
            return func;
        }

        auto &runtimeState = genericRuntimeStateFor(ownerModule);
        if (runtimeState.emittedInstances.count(instanceKey) != 0 ||
            runtimeState.inProgressInstances.count(instanceKey) != 0 ||
            findOwnerModuleFunction(symbolName)) {
            return func;
        }

        GenericFunctionEmissionGuard guard(runtimeState, instanceKey);
        auto genericArgs =
            buildAppliedStructGenericArgs(*typeDecl, structType, loc);
        std::vector<string> genericTypeParams;
        std::unordered_map<std::string, std::string> genericTypeParamBounds;
        genericTypeParams.reserve(typeDecl->typeParams.size());
        genericTypeParamBounds.reserve(typeDecl->typeParams.size());
        for (const auto &param : typeDecl->typeParams) {
            auto paramName = toStdString(param.localName);
            genericTypeParams.push_back(paramName);
            if (!param.boundTraitName.empty()) {
                genericTypeParamBounds.emplace(
                    paramName, toStdString(param.boundTraitName));
            }
        }

        auto resolvedModule = resolveGenericMethodInstance(
            global, templateUnit, methodDecl, string(symbolName),
            toStdString(structType->full_name),
            std::move(genericTypeParams),
            std::move(genericTypeParamBounds),
            templateUnit != unit ? templateUnit->interface() : nullptr,
            std::move(genericArgs));
        if (!resolvedModule || resolvedModule->functions().size() != 1) {
            internalError(
                loc,
                "generic struct method instance `" + symbolName +
                    "` did not resolve to exactly one function body",
                "This looks like a generic method resolve bug.");
        }

        auto *hirInstance = analyzeResolvedFunction(
            global, ownerModule, unit, *resolvedModule->functions().front(),
            lookupCache);
        if (!findOwnerModuleFunction(symbolName)) {
            ownerModule->addFunction(hirInstance);
        }
        guard.markCompleted();
        return func;
    }

    struct GenericMethodTemplateLookup {
        const ModuleInterface::TypeDecl *typeDecl = nullptr;
        const ModuleInterface::MethodTemplateDecl *methodTemplate = nullptr;
        const CompilationUnit *ownerUnit = nullptr;
        const AstFuncDecl *methodDecl = nullptr;

        bool found() const {
            return typeDecl != nullptr && methodTemplate != nullptr &&
                   ownerUnit != nullptr && methodDecl != nullptr;
        }
    };

    GenericMethodTemplateLookup lookupGenericMethodTemplate(
        StructType *structType, llvm::StringRef methodName,
        const location &loc) {
        GenericMethodTemplateLookup lookup;
        lookup.typeDecl = findVisibleTypeDeclForStructType(structType,
                                                           &lookup.ownerUnit);
        if (!lookup.typeDecl || !lookup.ownerUnit) {
            return lookup;
        }
        lookup.methodTemplate =
            findGenericMethodTemplateDecl(*lookup.typeDecl, methodName);
        if (!lookup.methodTemplate) {
            return lookup;
        }
        auto *structDecl = findStructDecl(lookup.ownerUnit, *lookup.typeDecl);
        lookup.methodDecl =
            findLocalStructMethodDecl(lookup.ownerUnit, structDecl, methodName);
        if (!structDecl || !lookup.methodDecl) {
            internalError(
                loc,
                "generic method `" + methodName.str() +
                    "` is missing its template AST",
                "This looks like a generic method registration bug.");
        }
        return lookup;
    }

    std::unordered_map<std::string, TypeClass *> resolveGenericMethodTypeArgs(
        const ModuleInterface::TypeDecl &typeDecl,
        const ModuleInterface::MethodTemplateDecl &methodTemplate,
        StructType *receiverStructType, const CallArgList &normalizedArgs,
        std::vector<TypeNode *> *explicitTypeArgs, const location &loc,
        const std::string &methodName,
        const ModuleInterface *ownerInterface) {
        const auto paramCount = methodTemplate.paramTypeNodes.size();
        if (methodTemplate.paramBindingKinds.size() != paramCount) {
            internalError(loc,
                          "generic method `" + methodName +
                              "` is missing parameter binding metadata",
                          "Rebuild interface collection before analyzing "
                          "generic method calls.");
        }

        std::vector<FormalCallArg> syntaxFormals;
        syntaxFormals.reserve(paramCount);
        for (std::size_t i = 0; i < paramCount; ++i) {
            const string *paramName =
                i < methodTemplate.paramNames.size()
                    ? &methodTemplate.paramNames[i]
                    : nullptr;
            syntaxFormals.push_back({paramName, nullptr,
                                     methodTemplate.paramBindingKinds[i],
                                     FormalCallArgKind::FunctionParameter, i});
        }
        auto orderedArgs = collectOrderedCallArgs(
            normalizedArgs, syntaxFormals,
            {loc, CallBindingTargetKind::FunctionCall, nullptr,
             !methodTemplate.paramNames.empty()});

        std::unordered_map<std::string, TypeClass *> selectedByName;
        selectedByName.reserve(methodTemplate.typeParams.size());
        for (const auto &param : methodTemplate.typeParams) {
            selectedByName.emplace(toStdString(param.localName), nullptr);
        }

        auto receiverGenericArgs =
            buildAppliedStructGenericArgs(typeDecl, receiverStructType, loc);
        for (const auto &param : typeDecl.typeParams) {
            auto paramName = toStdString(param.localName);
            auto found = receiverGenericArgs.find(paramName);
            if (found == receiverGenericArgs.end() || !found->second) {
                internalError(
                    loc,
                    "generic method `" + methodName +
                        "` is missing the receiver's concrete type for `" +
                        paramName + "`",
                    "This looks like a generic receiver instantiation bug.");
            }
            selectedByName[paramName] = found->second;
        }

        const auto methodTypeParamOffset = methodTemplate.enclosingTypeParamCount;
        const auto methodTypeParamCount =
            methodTemplate.typeParams.size() >= methodTypeParamOffset
                ? methodTemplate.typeParams.size() - methodTypeParamOffset
                : 0;

        if (explicitTypeArgs && !explicitTypeArgs->empty()) {
            if (explicitTypeArgs->size() != methodTypeParamCount) {
                error(loc,
                      "generic type argument count mismatch for `" +
                          methodName + "`: expected " +
                          std::to_string(methodTypeParamCount) + ", got " +
                          std::to_string(explicitTypeArgs->size()),
                      "Match the number of `[` `]` type arguments to the "
                      "generic parameter list.");
            }
            for (std::size_t i = 0; i < explicitTypeArgs->size(); ++i) {
                auto *type = requireType(
                    explicitTypeArgs->at(i), explicitTypeArgs->at(i)->loc,
                    "unknown generic type argument at index " +
                        std::to_string(i) + " for `" + methodName + "`");
                selectedByName[toStdString(
                    methodTemplate.typeParams[methodTypeParamOffset + i]
                        .localName)] = type;
            }
        } else if (methodTypeParamCount != 0) {
            for (std::size_t i = 0; i < paramCount; ++i) {
                auto *expr = requireNonCallExpr(orderedArgs[i].value);
                auto *actualType = expr ? expr->getType() : nullptr;
                if (!actualType) {
                    error(orderedArgs[i].loc,
                          "cannot infer generic type argument from a "
                          "non-value expression in `" +
                              methodName + "`");
                }
                inferGenericArgsFromPattern(methodTemplate.paramTypeNodes[i],
                                            actualType, selectedByName,
                                            orderedArgs[i].loc, methodName,
                                            ownerInterface);
            }
            for (std::size_t i = methodTypeParamOffset;
                 i < methodTemplate.typeParams.size(); ++i) {
                auto paramName =
                    toStdString(methodTemplate.typeParams[i].localName);
                auto *inferred = selectedByName[paramName];
                if (!inferred) {
                    error(loc,
                          "cannot infer generic type argument `" + paramName +
                              "` for `" + methodName + "`",
                          "Pass explicit type arguments like `" + methodName +
                              "[T](...)`.");
                }
            }
        }

        return selectedByName;
    }

    Function *declareGenericMethodInstance(
        StructType *structType,
        const ModuleInterface::MethodTemplateDecl &methodTemplate,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const std::vector<TypeClass *> &methodTypeArgs,
        const std::string &symbolName, const location &loc,
        const ModuleInterface *ownerInterface) {
        if (auto *existing = global->getObj(string(symbolName))) {
            auto *func = existing->as<Function>();
            if (!func) {
                internalError(
                    loc,
                    "generic method instance symbol `" + symbolName +
                        "` collides with a non-function global",
                    "This looks like a symbol declaration bug.");
            }
            return func;
        }
        if (!structType) {
            internalError(loc,
                          "generic method instance is missing its concrete "
                          "receiver type",
                          "This looks like a generic method instantiation bug.");
        }

        std::vector<TypeClass *> argTypes;
        argTypes.reserve(methodTemplate.paramTypeNodes.size() + 1);
        auto *selfPointee =
            methodTemplate.receiverAccess == AccessKind::GetSet
                ? static_cast<TypeClass *>(structType)
                : static_cast<TypeClass *>(typeMgr->createConstType(structType));
        argTypes.push_back(typeMgr->createPointerType(selfPointee));
        for (auto *paramTypeNode : methodTemplate.paramTypeNodes) {
            argTypes.push_back(substituteGenericSignatureType(
                paramTypeNode, genericArgs, loc,
                toStdString(methodTemplate.localName), ownerInterface));
        }
        auto *retType = substituteGenericSignatureType(
            methodTemplate.returnTypeNode, genericArgs, loc,
            toStdString(methodTemplate.localName), ownerInterface);
        auto paramBindingKinds = methodTemplate.paramBindingKinds;
        paramBindingKinds.insert(paramBindingKinds.begin(),
                                 BindingKind::Value);
        auto *funcType = typeMgr->getOrCreateFunctionType(
            argTypes, retType, paramBindingKinds, AbiKind::Native);
        if (!funcType) {
            internalError(loc,
                          "failed to build concrete method type for `" +
                              symbolName + "`",
                          "This looks like a generic method instantiation bug.");
        }

        auto *llvmFunc = llvm::Function::Create(
            getFunctionAbiLLVMType(*typeMgr, funcType, true),
            llvm::Function::ExternalLinkage, llvm::Twine(symbolName),
            global->module);
        annotateFunctionAbi(*llvmFunc, funcType->getAbiKind());
        auto *func = new Function(llvmFunc, funcType, methodTemplate.paramNames,
                                  true);
        global->addObj(string(symbolName), func);
        (void)methodTypeArgs;
        return func;
    }

    Function *ensureTraitImplMethodBinding(
        StructType *structType, const ModuleInterface::TraitImplDecl &implDecl,
        const ModuleInterface::MethodTemplateDecl &methodTemplate,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const location &loc, const ModuleInterface *ownerInterface) {
        if (!structType) {
            internalError(
                loc,
                "trait impl method binding is missing its concrete receiver "
                "type",
                "This looks like a trait impl lowering bug.");
        }

        auto methodKey =
            traitMethodSlotKey(implDecl.traitName, methodTemplate.localName);
        if (auto *existing =
                typeMgr->getMethodFunction(structType, toStringRef(methodKey))) {
            return existing;
        }

        auto *funcType = structType->getTraitMethodTypeByKey(toStringRef(methodKey));
        if (!funcType) {
            std::vector<TypeClass *> argTypes;
            argTypes.reserve(methodTemplate.paramTypeNodes.size() + 1);
            auto *selfPointee =
                methodTemplate.receiverAccess == AccessKind::GetSet
                    ? static_cast<TypeClass *>(structType)
                    : static_cast<TypeClass *>(typeMgr->createConstType(structType));
            argTypes.push_back(typeMgr->createPointerType(selfPointee));
            for (auto *paramTypeNode : methodTemplate.paramTypeNodes) {
                argTypes.push_back(substituteGenericSignatureType(
                    paramTypeNode, genericArgs, loc,
                    toStdString(methodTemplate.localName), ownerInterface));
            }
            auto *retType = substituteGenericSignatureType(
                methodTemplate.returnTypeNode, genericArgs, loc,
                toStdString(methodTemplate.localName), ownerInterface);
            auto paramBindingKinds = methodTemplate.paramBindingKinds;
            paramBindingKinds.insert(paramBindingKinds.begin(),
                                     BindingKind::Value);
            funcType = typeMgr->getOrCreateFunctionType(
                argTypes, retType, paramBindingKinds, AbiKind::Native);
            if (!funcType) {
                internalError(
                    loc,
                    "failed to build concrete trait impl method type for `" +
                        toStdString(methodTemplate.localName) + "`",
                    "This looks like a trait impl instantiation bug.");
            }
            structType->addTraitMethodType(
                toStringRef(implDecl.traitName),
                toStringRef(methodTemplate.localName), funcType,
                methodTemplate.paramNames);
        }

        auto llvmName = declarationsupport_impl::resolveTraitMethodSymbolName(
            structType, toStringRef(implDecl.traitName),
            toStringRef(methodTemplate.localName));
        auto *llvmFunc = global->module.getFunction(llvmName);
        if (!llvmFunc) {
            llvmFunc = llvm::Function::Create(
                getFunctionAbiLLVMType(*typeMgr, funcType, true),
                llvm::Function::ExternalLinkage, llvm::Twine(llvmName),
                global->module);
            annotateFunctionAbi(*llvmFunc, funcType->getAbiKind());
        }

        auto *func = new Function(llvmFunc, funcType, methodTemplate.paramNames,
                                  true);
        typeMgr->bindMethodFunction(structType, toStringRef(methodKey), func);
        if (!global->getObj(string(llvmName))) {
            global->addObj(string(llvmName), func);
        }
        return func;
    }

    Function *instantiateGenericTraitImplMethod(
        StructType *structType,
        const CompilationUnit::VisibleTraitImpl &visibleImpl,
        const ModuleInterface::MethodTemplateDecl &methodTemplate,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const location &loc) {
        auto *implDecl = visibleImpl.implDecl;
        auto *ownerUnit = traitImplOwnerUnit(visibleImpl);
        if (!implDecl || !ownerUnit) {
            internalError(
                loc,
                "generic trait impl method lookup is missing its owner "
                "metadata",
                "This looks like a trait impl instantiation bug.");
        }
        if (!methodTemplate.syntaxDecl) {
            internalError(
                loc,
                "generic trait impl method `" +
                    toStdString(methodTemplate.localName) +
                    "` is missing its template AST",
                "This looks like a trait impl registration bug.");
        }

        auto *func = ensureTraitImplMethodBinding(
            structType, *implDecl, methodTemplate, genericArgs, loc,
            traitImplOwnerInterface(visibleImpl));
        auto symbolName =
            func->getllvmValue()
                ? func->getllvmValue()->getName().str()
                : declarationsupport_impl::resolveTraitMethodSymbolName(
                      structType, toStringRef(implDecl->traitName),
                      toStringRef(methodTemplate.localName));
        auto instanceKey = buildTraitImplMethodInstanceKey(
            *implDecl, structType, ownerUnit,
            toStringRef(methodTemplate.localName), genericArgs, loc);
        const bool shouldEmit = claimGenericInstanceEmission(instanceKey);
        recordGenericInstance(instanceKey, ownerUnit,
                              shouldEmit ? std::vector<string>{string(symbolName)}
                                         : std::vector<string>{});
        if (!shouldEmit) {
            return func;
        }

        auto &runtimeState = genericRuntimeStateFor(ownerModule);
        if (runtimeState.emittedInstances.count(instanceKey) != 0 ||
            runtimeState.inProgressInstances.count(instanceKey) != 0 ||
            findOwnerModuleFunction(symbolName)) {
            return func;
        }

        GenericFunctionEmissionGuard guard(runtimeState, instanceKey);
        std::vector<string> genericTypeParams;
        std::unordered_map<std::string, std::string> genericTypeParamBounds;
        genericTypeParams.reserve(implDecl->typeParams.size());
        genericTypeParamBounds.reserve(implDecl->typeParams.size());
        for (const auto &param : implDecl->typeParams) {
            auto paramName = toStdString(param.localName);
            genericTypeParams.push_back(paramName);
            if (!param.boundTraitName.empty()) {
                genericTypeParamBounds.emplace(
                    paramName, toStdString(param.boundTraitName));
            }
        }

        auto resolvedModule = resolveGenericMethodInstance(
            global, ownerUnit, methodTemplate.syntaxDecl, string(symbolName),
            toStdString(structType->full_name), std::move(genericTypeParams),
            std::move(genericTypeParamBounds),
            traitImplOwnerInterface(visibleImpl), genericArgs);
        if (!resolvedModule || resolvedModule->functions().size() != 1) {
            internalError(
                loc,
                "generic trait impl method instance `" + symbolName +
                    "` did not resolve to exactly one function body",
                "This looks like a trait impl resolve bug.");
        }

        auto *hirInstance = analyzeResolvedFunction(
            global, ownerModule, unit, *resolvedModule->functions().front(),
            lookupCache);
        if (!findOwnerModuleFunction(symbolName)) {
            ownerModule->addFunction(hirInstance);
        }
        guard.markCompleted();
        return func;
    }

    void ensureVisibleTraitImplBodyMethods(
        const std::vector<CompilationUnit::VisibleTraitImpl> &visibleImpls,
        StructType *selfType, const location &loc,
        llvm::StringRef methodName = llvm::StringRef(),
        bool instantiateAllMethods = false,
        bool instantiateBodies = false) {
        for (const auto &visibleImpl : visibleImpls) {
            auto *implDecl = visibleImpl.implDecl;
            if (!implDecl || !implDecl->hasBody || implDecl->bodyMethods.empty()) {
                continue;
            }

            if (implDecl->isGeneric()) {
                auto genericArgs = resolveTraitImplGenericArgs(
                    *implDecl, selfType, loc, traitImplOwnerInterface(visibleImpl));
                if (instantiateAllMethods) {
                    for (const auto &method : implDecl->bodyMethods) {
                        if (instantiateBodies) {
                            instantiateGenericTraitImplMethod(
                                selfType, visibleImpl, method, genericArgs, loc);
                        } else {
                            (void)ensureTraitImplMethodBinding(
                                selfType, *implDecl, method, genericArgs, loc,
                                traitImplOwnerInterface(visibleImpl));
                        }
                    }
                    continue;
                }
                if (auto *methodTemplate =
                        findTraitImplBodyMethodTemplate(*implDecl, methodName)) {
                    if (instantiateBodies) {
                        instantiateGenericTraitImplMethod(
                            selfType, visibleImpl, *methodTemplate, genericArgs,
                            loc);
                    } else {
                        (void)ensureTraitImplMethodBinding(
                            selfType, *implDecl, *methodTemplate, genericArgs,
                            loc, traitImplOwnerInterface(visibleImpl));
                    }
                    continue;
                }
                continue;
            }

            if (instantiateAllMethods) {
                for (const auto &method : implDecl->bodyMethods) {
                    (void)ensureTraitImplMethodBinding(
                        selfType, *implDecl, method, {}, loc,
                        traitImplOwnerInterface(visibleImpl));
                }
                continue;
            }
            if (auto *methodTemplate =
                    findTraitImplBodyMethodTemplate(*implDecl, methodName)) {
                (void)ensureTraitImplMethodBinding(
                    selfType, *implDecl, *methodTemplate, {}, loc,
                    traitImplOwnerInterface(visibleImpl));
                continue;
            }
        }
    }

    Function *instantiateGenericMethod(
        StructType *structType, const GenericMethodTemplateLookup &lookup,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const std::vector<TypeClass *> &methodTypeArgs, const location &loc) {
        if (!lookup.found()) {
            internalError(loc,
                          "generic method instantiation is missing its "
                          "template metadata",
                          "This looks like a generic method lookup bug.");
        }

        auto symbolName = buildLocalGenericStructMethodInstanceSymbolName(
            structType, toStringRef(lookup.methodTemplate->localName),
            methodTypeArgs, loc);
        auto *func = declareGenericMethodInstance(
            structType, *lookup.methodTemplate, genericArgs, methodTypeArgs,
            symbolName, loc,
            lookup.ownerUnit != unit ? lookup.ownerUnit->interface() : nullptr);
        auto instanceKey = buildStructMethodInstanceKey(
            *lookup.typeDecl, structType, lookup.ownerUnit,
            toStringRef(lookup.methodTemplate->localName), methodTypeArgs, loc);
        const bool shouldEmit =
            claimGenericInstanceEmission(instanceKey);
        recordGenericInstance(instanceKey, lookup.ownerUnit,
                              shouldEmit ? std::vector<string>{string(symbolName)}
                                         : std::vector<string>{});
        if (!shouldEmit) {
            return func;
        }

        auto &runtimeState = genericRuntimeStateFor(ownerModule);
        if (runtimeState.emittedInstances.count(instanceKey) != 0 ||
            runtimeState.inProgressInstances.count(instanceKey) != 0 ||
            findOwnerModuleFunction(symbolName)) {
            return func;
        }

        GenericFunctionEmissionGuard guard(runtimeState, instanceKey);
        std::vector<string> genericTypeParams;
        genericTypeParams.reserve(lookup.typeDecl->typeParams.size() +
                                  lookup.methodTemplate->typeParams.size());
        std::unordered_map<std::string, std::string> genericTypeParamBounds;
        genericTypeParamBounds.reserve(lookup.typeDecl->typeParams.size() +
                                       lookup.methodTemplate->typeParams.size());
        for (const auto &param : lookup.typeDecl->typeParams) {
            auto paramName = toStdString(param.localName);
            genericTypeParams.push_back(paramName);
            if (!param.boundTraitName.empty()) {
                genericTypeParamBounds.emplace(
                    paramName, toStdString(param.boundTraitName));
            }
        }
        for (const auto &param : lookup.methodTemplate->typeParams) {
            auto paramName = toStdString(param.localName);
            genericTypeParams.push_back(paramName);
            if (!param.boundTraitName.empty()) {
                genericTypeParamBounds.emplace(
                    paramName, toStdString(param.boundTraitName));
            }
        }

        auto resolvedModule = resolveGenericMethodInstance(
            global, lookup.ownerUnit, lookup.methodDecl, string(symbolName),
            toStdString(structType->full_name),
            std::move(genericTypeParams),
            std::move(genericTypeParamBounds),
            lookup.ownerUnit != unit ? lookup.ownerUnit->interface() : nullptr,
            genericArgs);
        if (!resolvedModule || resolvedModule->functions().size() != 1) {
            internalError(
                loc,
                "generic method instance `" + symbolName +
                    "` did not resolve to exactly one function body",
                "This looks like a generic method resolve bug.");
        }

        auto *hirInstance = analyzeResolvedFunction(
            global, ownerModule, unit, *resolvedModule->functions().front(),
            lookupCache);
        if (!findOwnerModuleFunction(symbolName)) {
            ownerModule->addFunction(hirInstance);
        }
        guard.markCompleted();
        return func;
    }

    HIRExpr *borrowMethodReceiver(HIRExpr *receiver, const location &loc) {
        if (!receiver) {
            return nullptr;
        }
        auto binding = operatorResolver.resolveUnary(
            '&', receiver->getType(), isAddressable(receiver), loc);
        return makeHIR<HIRUnaryOper>(binding, receiver, binding.resultType, loc);
    }

    HIRExpr *lowerDirectGenericMethodCall(Function *methodFunc,
                                          HIRExpr *receiver,
                                          CallArgList normalizedArgs,
                                          const location &callLoc,
                                          llvm::StringRef methodName) {
        if (!methodFunc || !receiver) {
            internalError(
                callLoc,
                "generic method call is missing its callee or receiver",
                "This looks like a generic method lowering bug.");
        }
        auto *funcType = methodFunc->getType() ? methodFunc->getType()->as<FuncType>()
                                               : nullptr;
        if (!funcType) {
            internalError(callLoc,
                          "generic method call is missing its function type",
                          "This looks like a generic method instantiation bug.");
        }
        requireMethodReceiverCompatible(receiver->getType(), methodName,
                                        funcType, callLoc);

        const auto &paramTypes = funcType->getArgTypes();
        std::vector<FormalCallArg> formals;
        formals.reserve(paramTypes.size() > 0 ? paramTypes.size() - 1 : 0);
        for (std::size_t i = 1; i < paramTypes.size(); ++i) {
            const string *paramName =
                i - 1 < methodFunc->paramNames().size()
                    ? &methodFunc->paramNames()[i - 1]
                    : nullptr;
            formals.push_back({paramName, paramTypes[i],
                               funcType->getArgBindingKind(i),
                               FormalCallArgKind::FunctionParameter, i - 1});
        }
        auto boundArgs =
            bindCallArgs(normalizedArgs, formals,
                         {callLoc, CallBindingTargetKind::FunctionCall, nullptr,
                          !methodFunc->paramNames().empty()});

        std::vector<HIRExpr *> args;
        args.reserve(boundArgs.size() + 1);
        args.push_back(borrowMethodReceiver(receiver, callLoc));
        for (const auto &arg : boundArgs) {
            args.push_back(arg.expr);
        }

        auto *retType = funcType->getRetType();
        return makeHIR<HIRCall>(makeHIR<HIRValue>(methodFunc, callLoc),
                                std::move(args), retType, callLoc);
    }

    StructType *currentMethodParentType() {
        if (!resolved.isMethod()) {
            return nullptr;
        }
        return requireStructTypeByName(resolved.methodParentTypeName(),
                                       resolved.loc(), "method parent type");
    }

    bool hasInternalFieldAccess(StructType *ownerType) {
        auto *methodParent = currentMethodParentType();
        return ownerType && methodParent == ownerType;
    }

    TypeClass *applySlotConst(TypeClass *type) {
        if (!type) {
            return nullptr;
        }
        if (isConstQualifiedType(type)) {
            return type;
        }
        return typeMgr->createConstType(type);
    }

    TypeClass *projectStructFieldType(StructType *ownerStructType,
                                      TypeClass *ownerValueType,
                                      llvm::StringRef fieldName,
                                      TypeClass *fieldType) {
        if (!fieldType) {
            return nullptr;
        }
        if (!ownerStructType) {
            return fieldType;
        }

        bool requiresConstView = isConstQualifiedType(ownerValueType);
        if (!requiresConstView &&
            ownerStructType->getMemberAccess(fieldName) ==
                AccessKind::GetOnly &&
            !hasInternalFieldAccess(ownerStructType)) {
            requiresConstView = true;
        }
        return requiresConstView ? applySlotConst(fieldType) : fieldType;
    }

    TypeClass *projectArrayElementType(TypeClass *containerType,
                                       TypeClass *elementType) {
        if (!elementType) {
            return nullptr;
        }
        return isConstQualifiedType(containerType) ? applySlotConst(elementType)
                                                   : elementType;
    }

    TypeClass *projectTupleMemberType(TypeClass *tupleType,
                                      TypeClass *memberType) {
        if (!memberType) {
            return nullptr;
        }
        return isConstQualifiedType(tupleType) ? applySlotConst(memberType)
                                               : memberType;
    }

    TypeClass *getMethodReceiverPointee(FuncType *funcType) {
        if (!funcType || funcType->getArgTypes().empty()) {
            return nullptr;
        }
        return getRawPointerPointeeType(funcType->getArgTypes().front());
    }

    bool isReadOnlyTraitReceiverType(TypeClass *type) {
        if (auto *dynType = asUnqualified<DynTraitType>(type)) {
            return dynType->hasReadOnlyDataPtr();
        }
        return isConstQualifiedType(type);
    }

    void requireMethodReceiverCompatible(TypeClass *parentType,
                                         llvm::StringRef methodName,
                                         FuncType *funcType,
                                         const location &loc) {
        if (!parentType || !funcType) {
            return;
        }
        auto *receiverPointeeType = getMethodReceiverPointee(funcType);
        if (!receiverPointeeType) {
            internalError(
                loc, "method call is missing its receiver type information",
                "This looks like a compiler pipeline bug.");
        }
        if (isConstQualificationConvertible(receiverPointeeType, parentType)) {
            return;
        }

        error(loc,
              "set method `" + methodName.str() +
                  "` requires a writable receiver, got " +
                  describeResolvedType(parentType),
              "Call it on a writable value, or use a non-`set` method here.");
    }

    void requireMethodReceiverCompatible(HIRSelector *selector,
                                         FuncType *funcType,
                                         const location &loc) {
        if (!selector) {
            return;
        }
        auto *parentType =
            selector->getParent() ? selector->getParent()->getType() : nullptr;
        requireMethodReceiverCompatible(parentType,
                                        toStringRef(selector->getFieldName()),
                                        funcType, loc);
    }

    EntityRef classifyEntity(HIRExpr *expr) {
        if (!expr) {
            return EntityRef::invalid();
        }
        if (auto *valueExpr = dynamic_cast<HIRValue *>(expr)) {
            auto *value = valueExpr->getValue();
            if (!value) {
                return EntityRef::invalid();
            }
            if (auto *typeObject = value->as<TypeObject>()) {
                return EntityRef::type(typeObject->declaredType());
            }
            return EntityRef::object(value);
        }
        if (auto *type = expr->getType()) {
            return EntityRef::typedValue(type);
        }
        return EntityRef::invalid();
    }

    struct MemberLookupOwner {
        EntityRef entity;
        TypeClass *valueType = nullptr;
        TupleType *tupleType = nullptr;
        StructType *structType = nullptr;
    };

    struct MemberLookup {
        MemberLookupOwner owner;
        LookupResult result;
        std::optional<InjectedMemberBinding> injectedMember;
        std::string resolvedMethodName;
        std::vector<std::string> promotedPath;
        std::vector<std::vector<std::string>> ambiguousPromotedPaths;
        std::vector<std::string> ambiguousTraitNames;
    };

    struct MemberLookupAttempt {
        HIRExpr *parent = nullptr;
        MemberLookup lookup;
    };

    struct CallResolutionAttempt {
        HIRExpr *callee = nullptr;
        CallResolution resolution;
    };

    static bool isExplicitDerefSyntax(const AstNode *node) {
        auto *unary = dynamic_cast<const AstUnaryOper *>(node);
        return unary && unary->op == '*';
    }

    HIRExpr *implicitDeref(HIRExpr *expr, const location &loc) {
        if (!expr || !asUnqualified<PointerType>(expr->getType())) {
            return nullptr;
        }
        auto binding = operatorResolver.resolveUnary('*', expr->getType(),
                                                     isAddressable(expr), loc);
        return makeHIR<HIRUnaryOper>(binding, expr, binding.resultType, loc);
    }

    MemberLookupOwner classifyMemberOwner(HIRExpr *parent) {
        MemberLookupOwner owner;
        owner.entity = classifyEntity(parent);
        owner.valueType = owner.entity.valueType();
        owner.tupleType = asUnqualified<TupleType>(owner.valueType);
        owner.structType = asUnqualified<StructType>(owner.valueType);
        return owner;
    }

    LookupResult lookupDirectValueMember(
        const MemberLookupOwner &owner, const std::string &fieldName,
        std::string *resolvedMethodName = nullptr,
        std::vector<std::string> *ambiguousTraitNames = nullptr) {
        if (resolvedMethodName) {
            resolvedMethodName->clear();
        }
        if (ambiguousTraitNames) {
            ambiguousTraitNames->clear();
        }
        auto result = owner.entity.dot(fieldName);
        if (owner.tupleType) {
            TupleType::ValueTy member;
            if (owner.tupleType->getMember(llvm::StringRef(fieldName),
                                           member)) {
                result.kind = LookupResultKind::ValueField;
                result.resultEntity = EntityRef::typedValue(
                    projectTupleMemberType(owner.valueType, member.first));
                return result;
            }
            result.kind = LookupResultKind::NotFound;
            return result;
        }

        if (owner.structType) {
            if (auto *member =
                    owner.structType->getMember(llvm::StringRef(fieldName))) {
                result.kind = LookupResultKind::ValueField;
                result.resultEntity =
                    EntityRef::typedValue(projectStructFieldType(
                        owner.structType, owner.valueType,
                        llvm::StringRef(fieldName), member->first));
                return result;
            }
            if (auto *methodType = owner.structType->getMethodType(
                    llvm::StringRef(fieldName))) {
                if (resolvedMethodName) {
                    *resolvedMethodName = fieldName;
                }
                result.kind = LookupResultKind::Method;
                result.resultEntity = EntityRef::typedValue(methodType);
                return result;
            }
            auto traitMethods =
                owner.structType->findTraitMethodsByLocalName(fieldName);
            if (traitMethods.empty() && unit) {
                auto visibleImpls = unit->findVisibleTraitImpls(owner.structType);
                if (!visibleImpls.empty()) {
                    ensureVisibleTraitImplBodyMethods(
                        visibleImpls, owner.structType, location(),
                        llvm::StringRef(fieldName), false, false);
                    traitMethods =
                        owner.structType->findTraitMethodsByLocalName(fieldName);
                }
            }
            if (traitMethods.size() == 1) {
                auto *entry = traitMethods.front();
                if (resolvedMethodName) {
                    *resolvedMethodName = traitMethodSlotKey(
                        entry->traitName, entry->methodName);
                }
                result.kind = LookupResultKind::Method;
                result.resultEntity = EntityRef::typedValue(entry->funcType);
                return result;
            }
            if (traitMethods.size() > 1) {
                result.kind = LookupResultKind::Ambiguous;
                if (ambiguousTraitNames) {
                    ambiguousTraitNames->reserve(traitMethods.size());
                    for (const auto *entry : traitMethods) {
                        ambiguousTraitNames->push_back(
                            toStdString(entry->traitName));
                    }
                    std::sort(ambiguousTraitNames->begin(),
                              ambiguousTraitNames->end());
                    ambiguousTraitNames->erase(
                        std::unique(ambiguousTraitNames->begin(),
                                    ambiguousTraitNames->end()),
                        ambiguousTraitNames->end());
                }
                return result;
            }
            result.kind = LookupResultKind::NotFound;
            return result;
        }

        result.kind = LookupResultKind::NotFound;
        return result;
    }

    struct PromotedLookupState {
        StructType *structType = nullptr;
        TypeClass *valueType = nullptr;
        std::vector<std::string> path;
    };

    std::string formatPromotedMemberPath(const std::vector<std::string> &path,
                                         const std::string &fieldName) {
        std::string name;
        for (const auto &segment : path) {
            if (!name.empty()) {
                name += ".";
            }
            name += segment;
        }
        if (!name.empty()) {
            name += ".";
        }
        name += fieldName;
        return name;
    }

    void collectEmbeddedLookupStates(const MemberLookupOwner &owner,
                                     std::vector<PromotedLookupState> &states) {
        if (!owner.structType) {
            return;
        }
        for (const auto &member : owner.structType->getMembers()) {
            if (!owner.structType->isEmbeddedMember(member.first())) {
                continue;
            }
            auto *embeddedValueType =
                projectStructFieldType(owner.structType, owner.valueType,
                                       member.first(), member.second.first);
            auto *embeddedStructType =
                asUnqualified<StructType>(embeddedValueType);
            if (!embeddedStructType) {
                continue;
            }
            states.push_back({embeddedStructType,
                              embeddedValueType,
                              {member.first().str()}});
        }
    }

    void collectEmbeddedLookupStates(const PromotedLookupState &owner,
                                     std::vector<PromotedLookupState> &states) {
        if (!owner.structType) {
            return;
        }
        for (const auto &member : owner.structType->getMembers()) {
            if (!owner.structType->isEmbeddedMember(member.first())) {
                continue;
            }
            auto *embeddedValueType =
                projectStructFieldType(owner.structType, owner.valueType,
                                       member.first(), member.second.first);
            auto *embeddedStructType =
                asUnqualified<StructType>(embeddedValueType);
            if (!embeddedStructType) {
                continue;
            }
            auto path = owner.path;
            path.push_back(member.first().str());
            states.push_back(
                {embeddedStructType, embeddedValueType, std::move(path)});
        }
    }

    LookupResult lookupPromotedValueMember(
        const MemberLookupOwner &owner, const std::string &fieldName,
        std::vector<std::string> &promotedPath,
        std::vector<std::vector<std::string>> &ambiguousPaths,
        std::string *resolvedMethodName = nullptr,
        std::vector<std::string> *ambiguousTraitNames = nullptr) {
        LookupResult result = owner.entity.dot(fieldName);
        result.kind = LookupResultKind::NotFound;
        promotedPath.clear();
        ambiguousPaths.clear();
        if (resolvedMethodName) {
            resolvedMethodName->clear();
        }
        if (ambiguousTraitNames) {
            ambiguousTraitNames->clear();
        }
        if (!owner.structType) {
            return result;
        }

        std::vector<PromotedLookupState> frontier;
        collectEmbeddedLookupStates(owner, frontier);
        while (!frontier.empty()) {
            struct Candidate {
                LookupResult result;
                std::vector<std::string> path;
                std::string resolvedMethodName;
                std::vector<std::string> ambiguousTraitNames;
            };

            std::vector<Candidate> candidates;
            std::vector<PromotedLookupState> next;
            for (const auto &state : frontier) {
                MemberLookupOwner promotedOwner;
                promotedOwner.entity = EntityRef::typedValue(state.valueType);
                promotedOwner.valueType = state.valueType;
                promotedOwner.structType = state.structType;
                Candidate candidate;
                candidate.path = state.path;
                auto promotedLookup = lookupDirectValueMember(
                    promotedOwner, fieldName, &candidate.resolvedMethodName,
                    &candidate.ambiguousTraitNames);
                if (promotedLookup.kind == LookupResultKind::ValueField ||
                    promotedLookup.kind == LookupResultKind::Method) {
                    candidate.result = promotedLookup;
                    candidates.push_back(std::move(candidate));
                }
                collectEmbeddedLookupStates(state, next);
            }

            if (candidates.size() == 1) {
                promotedPath = candidates.front().path;
                if (resolvedMethodName) {
                    *resolvedMethodName = candidates.front().resolvedMethodName;
                }
                if (ambiguousTraitNames) {
                    *ambiguousTraitNames =
                        candidates.front().ambiguousTraitNames;
                }
                return candidates.front().result;
            }
            if (candidates.size() > 1) {
                result.kind = LookupResultKind::Ambiguous;
                for (const auto &candidate : candidates) {
                    ambiguousPaths.push_back(candidate.path);
                }
                return result;
            }
            frontier = std::move(next);
        }

        return result;
    }

    MemberLookup lookupMember(HIRExpr *parent, const std::string &fieldName,
                              const location &loc) {
        MemberLookup lookup;
        lookup.owner = classifyMemberOwner(parent);
        (void)loc;

        if (auto binding = resolveInjectedMemberBinding(
                typeMgr, lookup.owner.valueType, fieldName)) {
            lookup.result = lookup.owner.entity.dot(fieldName);
            lookup.result.kind = LookupResultKind::InjectedMember;
            lookup.result.resultEntity =
                EntityRef::typedValue(binding->resultType);
            lookup.injectedMember = binding;
            return lookup;
        }

        lookup.result = lookupDirectValueMember(
            lookup.owner, fieldName, &lookup.resolvedMethodName,
            &lookup.ambiguousTraitNames);
        if (lookup.result.kind != LookupResultKind::NotFound) {
            return lookup;
        }

        lookup.result = lookupPromotedValueMember(
            lookup.owner, fieldName, lookup.promotedPath,
            lookup.ambiguousPromotedPaths, &lookup.resolvedMethodName,
            &lookup.ambiguousTraitNames);
        return lookup;
    }

    MemberLookupAttempt lookupMemberWithImplicitDeref(
        HIRExpr *parent, const std::string &fieldName, const location &loc,
        bool allowImplicitDeref) {
        MemberLookupAttempt attempt;
        attempt.parent = parent;
        attempt.lookup = lookupMember(parent, fieldName, loc);
        if (!allowImplicitDeref ||
            attempt.lookup.result.kind != LookupResultKind::NotFound) {
            return attempt;
        }

        auto *derefParent = implicitDeref(parent, loc);
        if (!derefParent) {
            return attempt;
        }

        attempt.parent = derefParent;
        attempt.lookup = lookupMember(derefParent, fieldName, loc);
        return attempt;
    }

    HIRExpr *materializeMemberExpr(HIRExpr *parent,
                                   const std::string &fieldName,
                                   const MemberLookup &lookup,
                                   const location &loc,
                                   bool allowInjectedMember = false) {
        auto *current = parent;
        for (const auto &segment : lookup.promotedPath) {
            auto segmentOwner = classifyMemberOwner(current);
            auto segmentLookup = lookupDirectValueMember(segmentOwner, segment);
            if (segmentLookup.kind != LookupResultKind::ValueField) {
                internalError(
                    loc,
                    "promoted member path `" + segment +
                        "` did not resolve to a concrete field",
                    "This looks like a promoted-member lowering bug.");
            }
            current = makeHIR<HIRSelector>(
                current, segment, segmentLookup.resultEntity.valueType(), loc);
        }

        switch (lookup.result.kind) {
            case LookupResultKind::ValueField:
                return makeHIR<HIRSelector>(
                    current, fieldName, lookup.result.resultEntity.valueType(),
                    loc);
            case LookupResultKind::Method:
                return makeHIR<HIRSelector>(
                    current,
                    lookup.resolvedMethodName.empty() ? fieldName
                                                      : lookup.resolvedMethodName,
                    nullptr, loc, HIRSelectorKind::Method);
            case LookupResultKind::InjectedMember:
                if (allowInjectedMember) {
                    return nullptr;
                }
                error(
                    loc,
                    "injected member `" + fieldName +
                        "` can only be used as a direct call callee",
                    describeInjectedMemberHelp(current->getType(), fieldName));
            default:
                return nullptr;
        }
    }

    [[noreturn]] void diagnoseMemberLookupFailure(
        const MemberLookup &lookup, const std::string &fieldName,
        const location &loc, const std::string &ownerLabel = std::string()) {
        if (lookup.owner.entity.asType()) {
            auto typeName =
                ownerLabel.empty()
                    ? describeResolvedType(lookup.owner.entity.asType())
                    : ownerLabel;
            error(loc,
                  "unknown type member `" + typeName + "." + fieldName + "`",
                  "Static type members are not implemented yet.");
        }

        if (lookup.owner.tupleType) {
            error(loc, "unknown tuple field `" + fieldName + "`",
                  describeTupleFieldHelp(lookup.owner.tupleType));
        }

        if (lookup.result.kind == LookupResultKind::Ambiguous) {
            if (!lookup.ambiguousTraitNames.empty()) {
                std::string help = "Disambiguate with ";
                for (std::size_t i = 0; i < lookup.ambiguousTraitNames.size();
                     ++i) {
                    const auto &traitName = lookup.ambiguousTraitNames[i];
                    const auto dotPos = traitName.rfind('.');
                    const bool isCurrentModuleTrait =
                        unit && dotPos != std::string::npos &&
                        traitName.substr(0, dotPos) ==
                            toStdString(unit->exportNamespacePrefix());
                    const bool useReceiverPath =
                        dotPos == std::string::npos || isCurrentModuleTrait;
                    if (i != 0) {
                        help +=
                            i + 1 == lookup.ambiguousTraitNames.size() ? " or "
                                                                       : ", ";
                    } else {
                        help += useReceiverPath
                                    ? "an explicit trait path such as "
                                    : "a static trait-qualified call such as ";
                    }
                    if (useReceiverPath) {
                        auto ownerName = ownerLabel.empty()
                                             ? std::string("<expr>")
                                             : ownerLabel;
                        auto localTraitName =
                            dotPos == std::string::npos
                                ? traitName
                                : traitName.substr(dotPos + 1);
                        help += "`" + ownerName + "." + localTraitName + "." +
                                fieldName + "(...)`";
                    } else {
                        help += "`" + traitName + "." + fieldName +
                                "(<self>, ...)`";
                    }
                }
                help += ".";
                error(loc, "ambiguous trait method `" + fieldName + "`", help);
            }
            std::vector<std::string> suggestions;
            suggestions.reserve(lookup.ambiguousPromotedPaths.size());
            for (const auto &path : lookup.ambiguousPromotedPaths) {
                auto suggestion =
                    ownerLabel.empty()
                        ? formatPromotedMemberPath(path, fieldName)
                        : ownerLabel + "." +
                              formatPromotedMemberPath(path, fieldName);
                suggestions.push_back("`" + suggestion + "`");
            }
            std::string help = "Use an explicit embedded path such as ";
            for (std::size_t i = 0; i < suggestions.size(); ++i) {
                if (i != 0) {
                    help += i + 1 == suggestions.size() ? " or " : ", ";
                }
                help += suggestions[i];
            }
            help += ".";
            error(loc, "ambiguous promoted member `" + fieldName + "`", help);
        }

        if (lookup.owner.structType) {
            error(loc, "unknown struct field `" + fieldName + "`",
                  "Check the field name, or use a direct method call like "
                  "`obj.method(...)`.");
        }

        auto ownerType = lookup.owner.valueType
                             ? describeResolvedType(lookup.owner.valueType)
                             : std::string("<unknown type>");
        error(loc, "unknown member `" + ownerType + "." + fieldName + "`");
    }

    [[noreturn]] void diagnoseModuleNamespaceValueUse(
        const std::string &moduleName, const location &loc) {
        error(loc, "module namespaces can't be used as runtime values",
              "Access a concrete member like `" + moduleName +
                  ".func(...)` or `" + moduleName + ".Type(...)` instead.");
    }

    [[noreturn]] void diagnoseTraitNamespaceValueUse(
        const std::string &traitName, const location &loc) {
        error(loc, "trait namespaces can't be used as runtime values",
              "Call a concrete trait member like `" + traitName +
                  ".method(value, ...)` instead.");
    }

    [[noreturn]] void diagnoseModuleNamespaceCall(const std::string &moduleName,
                                                  const location &loc) {
        error(loc, "module `" + moduleName + "` does not support call syntax",
              "Call a concrete member like `" + moduleName +
                  ".func(...)` or `" + moduleName + ".Type(...)` instead.");
    }

    [[noreturn]] void diagnoseTraitNamespaceCall(const std::string &traitName,
                                                 const location &loc) {
        error(loc, "trait `" + traitName + "` does not support call syntax",
              "Use explicit trait-qualified call syntax like `" + traitName +
                  ".method(value, ...)`.");
    }

    [[noreturn]] void diagnoseGenericFunctionValueUse(
        const std::string &functionName, const location &loc) {
        error(loc,
              "generic function `" + functionName +
                  "` cannot be used as a runtime value before instantiation",
              "Call it directly, for example `" + functionName +
                  "[T](...)`, or instantiate it first with `@" + functionName +
                  "[T]` if you need a function pointer.");
    }

    [[noreturn]] void diagnoseGenericTypeApplyTarget(const location &loc) {
        error(loc,
              "explicit type arguments in expression contexts currently apply "
              "to top-level generic functions and generic type constructors only",
              "Use `name[T](...)` with a generic top-level function. "
              "Generic methods and other value-level specialization forms are "
              "not implemented in generic v0 yet.");
    }

    [[noreturn]] void diagnoseGenericTypeValueUse(const std::string &typeName,
                                                  const location &loc) {
        error(loc,
              "generic type template `" + typeName +
                  "` cannot be used as a runtime value directly",
              "Construct it with `" + typeName +
                  "[T](...)`, or write `" +
                  typeName + "[T]` in type positions.");
    }

    [[noreturn]] void diagnoseGenericTypeCall(const std::string &typeName,
                                              const location &loc) {
        error(loc,
              "generic type template `" + typeName +
                  "` cannot be constructed without explicit type arguments",
              "Write `" + typeName +
                  "[T](...)` with explicit type arguments. "
                  "Bare template names do not denote runtime constructible "
                  "types.");
    }

    [[noreturn]] void diagnoseGenericInstantiationPending(
        const std::string &functionName, const location &loc) {
        internalError(loc,
                      "generic function `" + functionName +
                          "` is missing its instantiation owner context",
                      "This looks like a generic runtime instantiation bug.");
    }

    [[noreturn]] void diagnoseGenericTypeInstantiationPending(
        const std::string &typeName, const location &loc) {
        internalError(loc,
                      "generic type `" + typeName +
                          "` is missing its instantiation owner context",
                      "This looks like a generic runtime instantiation bug.");
    }

    const ResolvedEntityRef *resolvedEntityBinding(const AstNode *node) const {
        if (auto *field = dynamic_cast<const AstField *>(node)) {
            return resolved.field(field);
        }
        if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
            return resolved.dotLike(dotLike);
        }
        if (auto *funcRef = dynamic_cast<const AstFuncRef *>(node)) {
            return resolved.functionRef(funcRef);
        }
        return nullptr;
    }

    const AstNode *callTargetNode(const AstFieldCall *node) const {
        if (!node) {
            return nullptr;
        }
        if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node->value)) {
            return typeApply->value;
        }
        return node->value;
    }

    std::vector<TypeNode *> *callExplicitTypeArgs(const AstFieldCall *node) const {
        if (!node) {
            return nullptr;
        }
        if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node->value)) {
            return typeApply->typeArgs;
        }
        return nullptr;
    }

    const AstNode *funcRefTargetNode(const AstFuncRef *node) const {
        if (!node) {
            return nullptr;
        }
        if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node->value)) {
            return typeApply->value;
        }
        return node->value;
    }

    std::vector<TypeNode *> *funcRefExplicitTypeArgs(const AstFuncRef *node) const {
        if (!node) {
            return nullptr;
        }
        if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node->value)) {
            return typeApply->typeArgs;
        }
        return nullptr;
    }

    const ResolvedEntityRef *resolvedGenericFunctionBinding(
        const AstNode *node) const {
        auto *binding = resolvedEntityBinding(node);
        if (!binding ||
            binding->kind() != ResolvedEntityRef::Kind::GenericFunction) {
            return nullptr;
        }
        return binding;
    }

    const ModuleInterface::FunctionDecl *
    resolvedGenericFunctionDecl(const AstNode *node) const {
        auto *binding = resolvedGenericFunctionBinding(node);
        return binding ? binding->functionDecl() : nullptr;
    }

    std::string describeGenericCallable(const AstNode *node) const {
        if (!node) {
            return "<generic function>";
        }
        if (auto *field = dynamic_cast<const AstField *>(node)) {
            return toStdString(field->name);
        }
        if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node)) {
            return describeGenericCallable(typeApply->value);
        }
        if (auto *funcRef = dynamic_cast<const AstFuncRef *>(node)) {
            return describeGenericCallable(funcRef->value);
        }
        if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
            return describeMemberOwnerSyntax(dotLike);
        }
        return "<generic function>";
    }

    const ResolvedEntityRef *resolvedTraitBinding(const AstNode *node) const {
        if (auto *field = dynamic_cast<const AstField *>(node)) {
            auto *binding = resolved.field(field);
            if (binding && binding->kind() == ResolvedEntityRef::Kind::Trait) {
                return binding;
            }
            return nullptr;
        }
        if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
            auto *binding = resolved.dotLike(dotLike);
            if (binding && binding->kind() == ResolvedEntityRef::Kind::Trait) {
                return binding;
            }
        }
        return nullptr;
    }

    const ModuleInterface::TraitDecl *requireVisibleTraitDecl(
        const ResolvedEntityRef *binding, const location &loc,
        const AstNode *syntax) {
        if (!binding || binding->kind() != ResolvedEntityRef::Kind::Trait) {
            internalError(loc,
                          "trait-qualified operation is missing its resolved "
                          "trait binding",
                          "Run name resolution before HIR lowering.");
        }
        if (!unit) {
            internalError(
                loc, "trait analysis requires compilation-unit context",
                "Compile trait-enabled code through the workspace pipeline.");
        }
        auto *traitDecl = unit->findVisibleTraitByResolvedName(
            binding->resolvedName());
        if (!traitDecl) {
            internalError(loc,
                          "resolved trait `" + toStdString(binding->resolvedName()) +
                              "` is missing from the visible interface graph",
                          "This looks like a trait interface materialization "
                          "bug.");
        }
        (void)syntax;
        return traitDecl;
    }

    const ModuleInterface::TraitDecl *requireVisibleDynTraitDecl(
        TypeClass *type, const location &loc, const std::string &context) {
        auto *dynType = asUnqualified<DynTraitType>(type);
        if (!dynType) {
            internalError(loc,
                          context + " expected a resolved `Trait dyn` type",
                          "This looks like a trait-object typing bug.");
        }
        if (!unit) {
            internalError(
                loc, "trait object analysis requires compilation-unit context",
                "Compile trait-enabled code through the workspace pipeline.");
        }
        auto *traitDecl =
            unit->findVisibleTraitByResolvedName(dynType->traitName());
        if (!traitDecl) {
            internalError(loc,
                          "resolved dyn trait `" +
                              toStdString(dynType->traitName()) +
                              "` is missing from the visible interface graph",
                          "This looks like a trait interface materialization "
                          "bug.");
        }
        return traitDecl;
    }

    const ModuleInterface::TraitDecl *findVisibleReceiverTraitDecl(
        llvm::StringRef rawName) const {
        if (!unit) {
            return nullptr;
        }
        auto lookup = unit->lookupTopLevelName(rawName.str());
        return lookup.isTrait() ? lookup.traitDecl : nullptr;
    }

    std::string resolveConcreteTraitMethodLookupName(
        StructType *receiverStructType,
        const ModuleInterface::TraitDecl &traitDecl,
        llvm::StringRef methodName) const {
        if (!receiverStructType) {
            return {};
        }
        auto traitMethodKey =
            traitMethodSlotKey(traitDecl.exportedName, methodName);
        if (receiverStructType->getTraitMethodTypeByKey(
                toStringRef(traitMethodKey)) ||
            typeMgr->getMethodFunction(receiverStructType,
                                       toStringRef(traitMethodKey))) {
            return traitMethodKey;
        }
        if (receiverStructType->getMethodType(methodName) ||
            typeMgr->getMethodFunction(receiverStructType, methodName)) {
            return methodName.str();
        }
        return {};
    }

    const ModuleInterface::TraitMethodDecl *findTraitMethodDecl(
        const ModuleInterface::TraitDecl &traitDecl, llvm::StringRef methodName,
        std::size_t *slotIndex = nullptr) {
        const auto methodNameText = methodName.str();
        for (std::size_t i = 0; i < traitDecl.methods.size(); ++i) {
            if (toStdString(traitDecl.methods[i].localName) == methodNameText) {
                if (slotIndex) {
                    *slotIndex = i;
                }
                return &traitDecl.methods[i];
            }
        }
        return nullptr;
    }

    void requireTraitMethodWritableReceiver(
        const ModuleInterface::TraitMethodDecl &traitMethod,
        TypeClass *receiverType, const location &loc,
        const std::string &hint) {
        if (traitMethod.receiverAccess == AccessKind::GetOnly ||
            !isReadOnlyTraitReceiverType(receiverType)) {
            return;
        }

        error(loc,
              "set trait method `" + toStdString(traitMethod.localName) +
                  "` requires a writable receiver, got " +
                  describeResolvedType(receiverType),
              hint);
    }

    TypeClass *resolveTraitMethodTypeBySpelling(const string &typeName,
                                                const location &loc,
                                                const std::string &context) {
        if (toStdString(typeName) == "void") {
            return nullptr;
        }
        return requireTypeByName(typeName, loc, context);
    }

    HIRExpr *buildConcreteTraitMethodCall(
        AstFieldCall *node, const ModuleInterface::TraitDecl &traitDecl,
        const ModuleInterface::TraitMethodDecl &traitMethod,
        HIRExpr *receiver, StructType *receiverStructType,
        llvm::StringRef methodLookupName, const CallArgList &callArgs,
        const location &calleeLoc) {
        if (!receiver || !receiverStructType) {
            internalError(calleeLoc,
                          "trait method call is missing its concrete receiver",
                          "This looks like a trait call lowering bug.");
        }

        if (receiverStructType->isAppliedTemplateInstance() &&
            methodLookupName ==
                llvm::StringRef(traitMethod.localName.tochara(),
                                traitMethod.localName.size()) &&
            receiverStructType->getMethodType(methodLookupName)) {
            (void)instantiateGenericStructMethod(receiverStructType,
                                                 methodLookupName, calleeLoc);
        }

        auto *callee = makeHIR<HIRSelector>(receiver, methodLookupName.str(),
                                            nullptr, calleeLoc,
                                            HIRSelectorKind::Method);

        std::vector<FormalCallArg> formals;
        formals.reserve(traitMethod.paramTypeSpellings.size());
        for (std::size_t i = 0; i < traitMethod.paramTypeSpellings.size();
             ++i) {
            auto *paramType = resolveTraitMethodTypeBySpelling(
                traitMethod.paramTypeSpellings[i], node->loc,
                "trait method call parameter type");
            const string *paramName =
                i < traitMethod.paramNames.size()
                    ? &traitMethod.paramNames[i]
                    : nullptr;
            formals.push_back({paramName, paramType,
                               traitMethod.paramBindingKinds[i],
                               FormalCallArgKind::FunctionParameter, i});
        }

        auto boundArgs = bindCallArgs(
            callArgs, formals,
            {node->loc, CallBindingTargetKind::FunctionCall, nullptr,
             !traitMethod.paramNames.empty()});

        std::vector<HIRExpr *> args;
        args.reserve(boundArgs.size());
        for (const auto &arg : boundArgs) {
            args.push_back(arg.expr);
        }

        auto *retType = resolveTraitMethodTypeBySpelling(
            traitMethod.returnTypeSpelling, node->loc,
            "trait method call return type");
        (void)traitDecl;
        return makeHIR<HIRCall>(callee, std::move(args), retType, node->loc);
    }

    FuncType *getOrCreateTraitDynSlotType(
        const ModuleInterface::TraitMethodDecl &traitMethod,
        const location &loc) {
        std::vector<TypeClass *> argTypes;
        std::vector<BindingKind> argBindingKinds;
        TypeClass *erasedByteType =
            traitMethod.receiverAccess == AccessKind::GetOnly
                ? static_cast<TypeClass *>(typeMgr->createConstType(u8Ty))
                : static_cast<TypeClass *>(u8Ty);
        argTypes.push_back(typeMgr->createPointerType(erasedByteType));
        argBindingKinds.push_back(BindingKind::Value);
        for (std::size_t i = 0; i < traitMethod.paramTypeSpellings.size();
             ++i) {
            auto *argType = resolveTraitMethodTypeBySpelling(
                traitMethod.paramTypeSpellings[i], loc,
                "trait dyn slot parameter type");
            argTypes.push_back(argType);
            argBindingKinds.push_back(traitMethod.paramBindingKinds[i]);
        }
        auto *retType = resolveTraitMethodTypeBySpelling(
            traitMethod.returnTypeSpelling, loc, "trait dyn slot return type");
        auto *slotType = typeMgr->getOrCreateFunctionType(
            argTypes, retType, std::move(argBindingKinds));
        if (!slotType) {
            internalError(loc,
                          "failed to build trait dyn slot signature for `" +
                              toStdString(traitMethod.localName) + "`",
                          "This looks like a trait-object signature bug.");
        }
        return slotType;
    }

    HIRExpr *materializeResolvedEntity(const ResolvedEntityRef *binding,
                                       const location &loc,
                                       const std::string &name) {
        if (!binding || !binding->valid()) {
            internalError(
                loc, "missing resolved identifier binding for `" + name + "`",
                "Run name resolution before HIR lowering.");
        }

        switch (binding->kind()) {
            case ResolvedEntityRef::Kind::LocalBinding:
                return makeHIR<HIRValue>(
                    requireBoundObject(binding->localBinding(), loc), loc);
            case ResolvedEntityRef::Kind::GlobalValue: {
                auto *obj = requireGlobalObject(binding->resolvedName(), loc,
                                                "global identifier");
                return makeHIR<HIRValue>(obj, loc);
            }
            case ResolvedEntityRef::Kind::GenericFunction:
                diagnoseGenericFunctionValueUse(name, loc);
            case ResolvedEntityRef::Kind::Type: {
                auto *type = requireTypeByName(binding->resolvedName(), loc,
                                               "type identifier");
                return makeHIR<HIRValue>(new TypeObject(type), loc);
            }
            case ResolvedEntityRef::Kind::GenericType:
                diagnoseGenericTypeValueUse(name, loc);
            case ResolvedEntityRef::Kind::Trait:
                diagnoseTraitNamespaceValueUse(
                    toStdString(binding->resolvedName()), loc);
            case ResolvedEntityRef::Kind::Invalid:
                break;
        }
        internalError(loc,
                      "unsupported resolved entity kind for `" + name + "`",
                      "This looks like a compiler pipeline bug.");
    }

    CallResolution resolveCall(HIRExpr *callee, CallArgList callArgs,
                               const location &loc) {
        auto resolution = classifyEntity(callee).applyCall(std::move(callArgs));

        if (auto *calleeValue = dynamic_cast<HIRValue *>(callee)) {
            if (auto *typeObject = calleeValue->getValue()->as<TypeObject>()) {
                auto *declaredType = typeObject->declaredType();
                auto *structType = asUnqualified<StructType>(declaredType);
                if (structType) {
                    if (structType->isOpaque()) {
                        error(loc,
                              "opaque struct `" +
                                  describeResolvedType(structType) +
                                  "` cannot be constructed by value",
                              "Use `" + describeResolvedType(structType) +
                                  "*` from an API that owns the storage "
                                  "instead. Opaque structs do not expose "
                                  "fields or value layout.");
                    }
                    resolution.kind = CallResolutionKind::ConstructorCall;
                    resolution.resultEntity = EntityRef::typedValue(structType);
                    return resolution;
                }
                resolution.kind = CallResolutionKind::NotCallable;
                resolution.callee = EntityRef::type(declaredType);
                return resolution;
            }
        }

        if (auto *selector = dynamic_cast<HIRSelector *>(callee);
            selector && selector->isMethodSelector()) {
            auto *structType = selector->getParent()
                                   ? asUnqualified<StructType>(
                                         selector->getParent()->getType())
                                   : nullptr;
            if (!structType) {
                internalError(loc,
                              "selector call parent must be a struct value");
            }
            auto methodName = toStringRef(selector->getFieldName());
            auto *methodFunc = typeMgr->getMethodFunction(structType, methodName);
            if (structType->isAppliedTemplateInstance() &&
                structType->getMethodType(methodName)) {
                methodFunc =
                    instantiateGenericStructMethod(structType, methodName,
                                                   loc);
            }
            if (unit && structType->getTraitMethodTypeByKey(methodName)) {
                auto visibleImpls = unit->findVisibleTraitImpls(structType);
                if (!visibleImpls.empty()) {
                    ensureVisibleTraitImplBodyMethods(
                        visibleImpls, structType, loc, methodName, false, true);
                    methodFunc =
                        typeMgr->getMethodFunction(structType, methodName);
                }
            }
            auto *funcType = methodFunc ? methodFunc->getType()->as<FuncType>()
                                        : getStructMethodTypeByKey(structType,
                                                                   methodName);
            if (!funcType) {
                internalError(loc, "unknown struct method");
            }
            requireMethodReceiverCompatible(selector, funcType, loc);
            resolution.kind = CallResolutionKind::FunctionCall;
            resolution.callType = funcType;
            resolution.paramNames =
                methodFunc && !methodFunc->paramNames().empty()
                    ? &methodFunc->paramNames()
                    : getStructMethodParamNamesByKey(structType, methodName);
            resolution.argOffset = getMethodCallArgOffset(selector, funcType);
            if (auto *retType = funcType->getRetType()) {
                resolution.resultEntity = EntityRef::typedValue(retType);
            }
            return resolution;
        }

        if (auto *func = getDirectFunctionCallee(callee)) {
            auto *funcType = func->getType()->as<FuncType>();
            resolution.kind = CallResolutionKind::FunctionCall;
            resolution.callType = funcType;
            resolution.paramNames = func && !func->paramNames().empty()
                                        ? &func->paramNames()
                                        : nullptr;
            if (funcType && funcType->getRetType()) {
                resolution.resultEntity =
                    EntityRef::typedValue(funcType->getRetType());
            }
            return resolution;
        }

        if (auto *pointerTarget = getFunctionPointerTarget(callee->getType())) {
            resolution.kind = CallResolutionKind::FunctionPointerCall;
            resolution.callType = pointerTarget;
            if (pointerTarget->getRetType()) {
                resolution.resultEntity =
                    EntityRef::typedValue(pointerTarget->getRetType());
            }
            return resolution;
        }

        if (auto *arrayType = asUnqualified<ArrayType>(callee->getType())) {
            resolution.kind = CallResolutionKind::ArrayIndex;
            resolution.resultEntity =
                EntityRef::typedValue(projectArrayElementType(
                    callee->getType(), arrayType->getElementType()));
            return resolution;
        }
        if (auto *indexableType =
                asUnqualified<IndexablePointerType>(callee->getType())) {
            resolution.kind = CallResolutionKind::ArrayIndex;
            resolution.resultEntity =
                EntityRef::typedValue(indexableType->getElementType());
            return resolution;
        }

        resolution.kind = CallResolutionKind::NotCallable;
        return resolution;
    }

    CallResolutionAttempt resolveCallWithImplicitDeref(
        HIRExpr *callee, const CallArgList &callArgs, const location &loc,
        bool allowImplicitDeref) {
        CallResolutionAttempt attempt;
        attempt.callee = callee;
        attempt.resolution = resolveCall(callee, callArgs, loc);
        if (!allowImplicitDeref ||
            attempt.resolution.kind != CallResolutionKind::NotCallable) {
            return attempt;
        }

        auto *derefCallee = implicitDeref(callee, loc);
        if (!derefCallee) {
            return attempt;
        }

        attempt.callee = derefCallee;
        attempt.resolution = resolveCall(derefCallee, callArgs, loc);
        return attempt;
    }

    [[noreturn]] void diagnoseCallFailure(HIRExpr *callee, const location &loc,
                                          const CallResolution &resolution) {
        if (auto *type = resolution.callee.asType()) {
            if (!asUnqualified<StructType>(type)) {
                error(loc,
                      "constructor calls currently support struct types only",
                      "Use a struct type like `Vec2(...)`. Numeric conversion "
                      "uses `cast[T](expr)`.");
            }
        }

        (void)callee;
        error(loc, "this expression does not support call syntax",
              "Only functions, function pointers, struct constructors, fixed "
              "arrays, and indexable pointers support `(...)` here.");
    }

    Function *requireDeclaredFunction(const location &loc) {
        if (!resolved.hasDeclaredFunction()) {
            internalError(
                loc, "resolved function is missing its stable symbol identity",
                "This looks like a compiler pipeline bug.");
        }
        if (resolved.isMethod()) {
            auto *structType = requireStructTypeByName(
                resolved.methodParentTypeName(), loc, "method parent type");
            auto *func = typeMgr->getMethodFunction(
                structType, toStringRef(resolved.functionName()));
            if (func) {
                return func;
            }
            if (resolved.decl() && !resolved.decl()->hasTypeParams() &&
                toStdString(resolved.decl()->name) !=
                    toStdString(resolved.functionName())) {
                if (auto *instantiatedMethod = typeMgr->getMethodFunction(
                        structType, toStringRef(resolved.decl()->name))) {
                    return instantiatedMethod;
                }
            }
            if (resolved.decl() &&
                (resolved.decl()->hasTypeParams() ||
                 toStdString(resolved.decl()->name) !=
                     toStdString(resolved.functionName()))) {
                return requireGlobalFunction(resolved.functionName(), loc,
                                             "generic method instance");
            }
            if (!func) {
                internalError(loc,
                              "resolved method `" +
                                  toStdString(resolved.methodParentTypeName()) +
                                  "." + toStdString(resolved.functionName()) +
                                  "` is missing from the current type table",
                              "Rebuild declarations before reusing this "
                              "resolved module.");
            }
            return func;
        }
        return requireGlobalFunction(resolved.functionName(), loc,
                                     "function declaration");
    }

    HIRExpr *requireExpr(AstNode *node, TypeClass *expectedType = nullptr) {
        auto *expr = analyzeExpr(node, expectedType);
        if (!expr) {
            error(node ? node->loc : location(),
                  "expression did not produce a value");
        }
        return expr;
    }

    HIRExpr *coerceNumericExpr(HIRExpr *expr, TypeClass *targetType,
                               const location &loc, bool explicitRequest) {
        return coerceNumericInitializerExpr(typeMgr, ownerModule, expr,
                                            targetType, loc, explicitRequest);
    }

    HIRExpr *coercePointerExpr(HIRExpr *expr, TypeClass *targetType,
                               const location &loc, bool explicitCast = false) {
        return coercePointerInitializerExpr(typeMgr, ownerModule, expr,
                                            targetType, loc, explicitCast);
    }

    HIRExpr *coerceBitCopyExpr(HIRExpr *expr, TypeClass *targetType,
                               const location &loc) {
        if (!expr || !targetType) {
            return expr;
        }
        auto *sourceType = expr->getType();
        if (!sourceType || sourceType == targetType) {
            return expr;
        }
        if (!canExplicitBitCopy(targetType, sourceType)) {
            error(loc,
                  "raw bit-copy is not available from `" +
                      describeResolvedType(sourceType) + "` to `" +
                      describeResolvedType(targetType) + "`",
                  bitCopyHint());
        }
        return makeHIR<HIRBitCast>(expr, targetType, loc);
    }

    HIRExpr *analyzeTraitObjectCast(AstCastExpr *node, TypeClass *targetType) {
        auto *dynType = asUnqualified<DynTraitType>(targetType);
        if (!dynType) {
            internalError(node ? node->loc : location(),
                          "trait-object cast is missing its dyn target type",
                          "This looks like a cast-analysis bug.");
        }

        auto *borrowSyntax = node && node->value ? node->value->as<AstUnaryOper>()
                                                 : nullptr;
        if (!borrowSyntax || borrowSyntax->op != '&' || !borrowSyntax->expr) {
            error(node ? node->loc : location(),
                  "trait object construction requires an explicit borrow",
                  "Write `cast[" + describeResolvedType(targetType) +
                      "](&value)`. Implicit boxing and temporary capture are "
                      "not supported.");
        }

        auto *source = requireNonCallExpr(borrowSyntax->expr);
        if (!isAddressable(source)) {
            error(borrowSyntax->expr->loc,
                  "trait object construction expects an addressable source",
                  "Borrow a variable, field, pointer-backed value like "
                  "`self`, or array element. Temporaries cannot become "
                  "`Trait dyn`.");
        }

        auto *traitDecl = requireVisibleDynTraitDecl(
            targetType, node->loc, "trait object construction");
        auto *borrowedSource = source;
        auto *selfType = asUnqualified<StructType>(borrowedSource->getType());
        if (!selfType) {
            auto *sourcePointer = asUnqualified<PointerType>(source->getType());
            auto *pointeeType =
                sourcePointer ? sourcePointer->getPointeeType() : nullptr;
            selfType = asUnqualified<StructType>(pointeeType);
            if (selfType) {
                borrowedSource = implicitDeref(source, borrowSyntax->expr->loc);
                if (!borrowedSource || !isAddressable(borrowedSource)) {
                    internalError(
                        borrowSyntax->expr->loc,
                        "trait object construction failed to materialize a "
                        "pointer-backed borrow source",
                        "This looks like a trait-object borrow lowering bug.");
                }
            }
        }
        if (!selfType) {
            error(borrowSyntax->expr->loc,
                  "trait object construction expects a concrete struct value "
                  "for trait `" +
                      toStdString(traitDecl->exportedName) + "`",
                  "Only struct types can currently satisfy "
                  "`impl Trait for Type { ... }`.");
        }

        auto visibleImpls =
            unit->findVisibleTraitImpls(traitDecl->exportedName, selfType);
        if (visibleImpls.empty()) {
            error(borrowSyntax->expr->loc,
                  "type `" + describeResolvedType(selfType) +
                      "` does not implement trait `" +
                      toStdString(traitDecl->exportedName) + "`",
                  "Add `impl " + toStdString(traitDecl->exportedName) +
                      " for " + describeResolvedType(selfType) +
                      " { ... }` in a visible module before constructing `" +
                      describeResolvedType(targetType) + "`.");
        }
        ensureVisibleTraitImplBodyMethods(visibleImpls, selfType,
                                          borrowSyntax->expr->loc,
                                          llvm::StringRef(), true, true);

        TypeClass *resultDynType = typeMgr->createDynTraitType(
            dynType->traitName(),
            dynType->hasReadOnlyDataPtr() ||
                isConstQualifiedType(borrowedSource->getType()));

        return makeHIR<HIRTraitObjectCast>(borrowedSource, resultDynType,
                                           node->loc);
    }

    HIRExpr *analyzeCastExpr(AstCastExpr *node) {
        if (!node || !node->targetType || !node->value) {
            error(
                node ? node->loc : location(),
                "builtin cast requires exactly one target type and one value");
        }

        auto *targetType = requireType(node->targetType, node->targetType->loc,
                                       "unknown cast target type");
        if (asUnqualified<DynTraitType>(targetType)) {
            return analyzeTraitObjectCast(node, targetType);
        }
        auto *value = requireNonCallExpr(node->value);
        if (isNullLiteralExpr(value)) {
            if (!isPointerLikeType(targetType)) {
                error(node->loc,
                      "unsupported builtin cast from `null` to `" +
                          describeResolvedType(targetType) + "`",
                      nullLiteralHint());
            }
            return makeHIR<HIRNullLiteral>(targetType, node->loc);
        }
        auto *sourceType = value ? value->getType() : nullptr;
        if (!sourceType) {
            error(node->loc,
                  "cast source does not produce a typed runtime value",
                  "Cast a runtime value like `cast[i32](x)` instead of a type "
                  "or namespace.");
        }

        if (!isBuiltinCastType(sourceType) || !isBuiltinCastType(targetType)) {
            error(node->loc,
                  "unsupported builtin cast from `" +
                      describeResolvedType(sourceType) + "` to `" +
                      describeResolvedType(targetType) + "`",
                  castDomainHint());
        }

        if (sourceType == targetType) {
            return value;
        }

        if (canExplicitNumericConversion(targetType, sourceType)) {
            return makeHIR<HIRNumericCast>(value, targetType, true, node->loc);
        }

        auto *pointerCast =
            coercePointerExpr(value, targetType, node->loc, true);
        if (pointerCast && pointerCast->getType() == targetType) {
            return pointerCast;
        }

        error(node->loc,
              "unsupported builtin cast from `" +
                  describeResolvedType(sourceType) + "` to `" +
                  describeResolvedType(targetType) + "`",
              numericConversionHint() + " " + pointerConversionHint() + " " +
                  bitCopyHint());
    }

    std::uint64_t requireSizeofByteCount(TypeClass *type, const location &loc,
                                         const std::string &context) {
        auto *storageType = materializeValueType(typeMgr, type);
        if (!storageType) {
            error(loc, context,
                  "Use a concrete type like `sizeof[i32]()` or a typed runtime "
                  "value like `sizeof(x)`.");
        }
        if (storageType->as<FuncType>()) {
            error(loc, "`sizeof` does not support function types",
                  "Use an explicit function pointer type such as "
                  "`sizeof[(i32:)]()` or a function pointer value if you need "
                  "pointer size.");
        }

        const auto byteCount = typeMgr->getTypeAllocSize(storageType);
        if (byteCount == 0) {
            error(loc, "`sizeof` requires a concrete type with known layout",
                  "Opaque extern structs, bare functions, and untyped `null` "
                  "do not have a compile-time size here.");
        }
        return byteCount;
    }

    HIRExpr *analyzeSizeofExpr(AstSizeofExpr *node) {
        if (!node || (!node->hasTypeOperand() && !node->hasValueOperand())) {
            error(node ? node->loc : location(),
                  "builtin `sizeof` requires exactly one operand");
        }

        TypeClass *operandType = nullptr;
        if (node->hasTypeOperand()) {
            operandType = requireType(node->targetType, node->targetType->loc,
                                      "unknown `sizeof` target type");
        } else {
            auto *value = requireNonCallExpr(node->value);
            operandType = value ? value->getType() : nullptr;
            if (!operandType) {
                error(node->loc,
                      "`sizeof` value operand must have a concrete type",
                      "Use `sizeof[T]()` for a type, or pass a typed runtime "
                      "value.");
            }
        }

        const auto byteCount = requireSizeofByteCount(
            operandType, node->loc,
            node->hasTypeOperand()
                ? "`sizeof` target type is not sized"
                : "`sizeof` value operand does not have a known size");
        const auto usizeBytes = typeMgr->getTypeAllocSize(usizeTy);
        const auto usizeBits = static_cast<unsigned>(usizeBytes * 8);
        if (usizeBits < 64 &&
            byteCount > ((std::uint64_t{1} << usizeBits) - 1)) {
            error(
                node->loc,
                "`sizeof` result does not fit in `usize` for the active target",
                "Use a target with a wider pointer size, or avoid requesting "
                "layouts this large.");
        }

        return makeHIR<HIRValue>(new ConstVar(usizeTy, byteCount), node->loc);
    }

    HIRExpr *requireNonCallExpr(AstNode *node,
                                TypeClass *expectedType = nullptr) {
        auto *expr = requireExpr(node, expectedType);
        rejectNonCallMethodSelector(typeMgr, expr);
        return expr;
    }

    CallArgSpec normalizeCallArg(AstNode *node, const location &callLoc) {
        if (!node) {
            error(callLoc, "call argument is missing");
        }

        CallArgSpec spec;
        spec.syntax = node;
        spec.loc = node->loc;
        AstNode *value = node;
        if (auto *namedArg = dynamic_cast<AstNamedCallArg *>(node)) {
            spec.name = toStdString(namedArg->name);
            value = namedArg->value;
        }
        if (auto *refExpr = dynamic_cast<AstRefExpr *>(value)) {
            spec.bindingKind = BindingKind::Ref;
            value = refExpr->expr;
        }
        if (!value) {
            error(node->loc, "call argument is missing its value",
                  "Write arguments like `f(x)`, `f(name=x)`, or `f(ref x)`.");
        }
        spec.value = value;
        return spec;
    }

    CallArgList normalizeCallArgs(const std::vector<AstNode *> *rawArgs,
                                  const location &callLoc) {
        CallArgList normalized;
        if (!rawArgs || rawArgs->empty()) {
            return normalized;
        }
        normalized.reserve(rawArgs->size());
        for (auto *arg : *rawArgs) {
            normalized.push_back(normalizeCallArg(arg, callLoc));
        }
        return normalized;
    }

    static bool isNamedCallArg(const CallArgSpec &arg) {
        return arg.name.has_value();
    }

    static std::string formatAvailableNames(
        const std::vector<std::string> &names, const std::string &noun) {
        if (names.empty()) {
            return "No " + noun + " names are available here.";
        }
        std::ostringstream out;
        out << "Available " << noun << " names are ";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << "`" << names[i] << "`";
        }
        out << ".";
        return out.str();
    }

    enum class FormalCallArgKind {
        ConstructorField,
        FunctionParameter,
        ArrayIndex,
    };

    struct FormalCallArg {
        const string *name = nullptr;
        TypeClass *type = nullptr;
        BindingKind bindingKind = BindingKind::Value;
        FormalCallArgKind kind = FormalCallArgKind::FunctionParameter;
        std::size_t index = 0;
    };

    enum class CallBindingTargetKind {
        Constructor,
        FunctionCall,
        ArrayIndex,
    };

    struct CallBindingOptions {
        location callLoc;
        CallBindingTargetKind targetKind = CallBindingTargetKind::FunctionCall;
        TypeClass *targetType = nullptr;
        bool allowNamedArgs = false;
    };

    struct BoundCallArg {
        CallArgSpec spec;
        HIRExpr *expr = nullptr;
        TypeClass *expectedType = nullptr;
        BindingKind bindingKind = BindingKind::Value;
        std::size_t index = 0;
    };

    static const string *formalCallArgName(const FormalCallArg &formal) {
        return formal.name;
    }

    static std::string describeFormalCallArg(const FormalCallArg &formal) {
        switch (formal.kind) {
            case FormalCallArgKind::ConstructorField:
                return "field `" +
                       (formal.name ? toStdString(*formal.name)
                                    : std::string()) +
                       "`";
            case FormalCallArgKind::FunctionParameter:
                if (formal.name && !formal.name->empty()) {
                    return "parameter `" + toStdString(*formal.name) + "`";
                }
                return "parameter at index " + std::to_string(formal.index);
            case FormalCallArgKind::ArrayIndex:
                return "index at position " + std::to_string(formal.index);
        }
        return "parameter";
    }

    static std::string formalCallArgTypeMismatchContext(
        const FormalCallArg &formal) {
        switch (formal.kind) {
            case FormalCallArgKind::ConstructorField:
                return "constructor field type mismatch for `" +
                       (formal.name ? toStdString(*formal.name)
                                    : std::string()) +
                       "`";
            case FormalCallArgKind::FunctionParameter:
                return "call argument type mismatch at index " +
                       std::to_string(formal.index);
            case FormalCallArgKind::ArrayIndex:
                return "array index type mismatch at index " +
                       std::to_string(formal.index);
        }
        return "call argument type mismatch";
    }

    static std::string formalCallArgMissingRefHint(
        const FormalCallArg &formal) {
        if (formal.kind == FormalCallArgKind::FunctionParameter &&
            formal.name && !formal.name->empty()) {
            return "Pass it as `ref " + toStdString(*formal.name) +
                   " = value` for named calls, or `ref value` positionally.";
        }
        return "Pass it as `ref value`.";
    }

    std::string describeCallTarget(const CallBindingOptions &options) {
        switch (options.targetKind) {
            case CallBindingTargetKind::Constructor:
                return "constructor `" +
                       describeResolvedType(options.targetType) + "`";
            case CallBindingTargetKind::FunctionCall:
                return options.allowNamedArgs ? "function call"
                                              : "this call target";
            case CallBindingTargetKind::ArrayIndex:
                return "array indexing";
        }
        return "this call target";
    }

    static const char *callBindingNameKind(const CallBindingOptions &options) {
        switch (options.targetKind) {
            case CallBindingTargetKind::Constructor:
                return "field";
            case CallBindingTargetKind::FunctionCall:
                return "parameter";
            case CallBindingTargetKind::ArrayIndex:
                return "index";
        }
        return "parameter";
    }

    std::string callBindingCountMismatchLabel(
        const CallBindingOptions &options) {
        switch (options.targetKind) {
            case CallBindingTargetKind::Constructor:
                return "constructor argument count mismatch for `" +
                       describeResolvedType(options.targetType) + "`";
            case CallBindingTargetKind::FunctionCall:
                return "call argument count mismatch";
            case CallBindingTargetKind::ArrayIndex:
                return "array index arity mismatch";
        }
        return "call argument count mismatch";
    }

    std::string disallowRefMessage(const FormalCallArg &formal,
                                   const CallBindingOptions &options) {
        switch (options.targetKind) {
            case CallBindingTargetKind::Constructor:
                return "constructor arguments do not accept `ref`";
            case CallBindingTargetKind::ArrayIndex:
                return "array indexing does not accept `ref` arguments";
            case CallBindingTargetKind::FunctionCall:
                return "value " + describeFormalCallArg(formal) +
                       " cannot be passed with `ref`";
        }
        return "value cannot be passed with `ref`";
    }

    static const char *disallowRefHint(const CallBindingOptions &options) {
        switch (options.targetKind) {
            case CallBindingTargetKind::Constructor:
                return "Constructors copy field values. Remove `ref` from this "
                       "argument.";
            case CallBindingTargetKind::ArrayIndex:
                return "Use positional indices like `a(i, j)` without `ref`.";
            case CallBindingTargetKind::FunctionCall:
                return "Remove `ref` and pass the value directly.";
        }
        return "Remove `ref` and pass the value directly.";
    }

    std::string formatAvailableFormalNames(
        const std::vector<FormalCallArg> &formals,
        const CallBindingOptions &options) {
        std::vector<std::string> names;
        names.reserve(formals.size());
        for (const auto &formal : formals) {
            names.push_back(formal.name ? toStdString(*formal.name)
                                        : std::string());
        }
        return formatAvailableNames(names, callBindingNameKind(options));
    }

    CallArgList collectOrderedCallArgs(
        const CallArgList &normalizedArgs,
        const std::vector<FormalCallArg> &formals,
        const CallBindingOptions &options) {
        CallArgList normalized = normalizedArgs;
        if (normalized.empty()) {
            return normalized;
        }

        bool hasNamedArgs = false;
        for (const auto &arg : normalized) {
            if (isNamedCallArg(arg)) {
                hasNamedArgs = true;
            }
        }

        if (!hasNamedArgs) {
            return normalized;
        }

        if (!options.allowNamedArgs) {
            error(options.callLoc,
                  "named arguments are not supported for " +
                      describeCallTarget(options),
                  "Use positional arguments for this call target.");
        }

        std::unordered_map<std::string, std::size_t> indexByName;
        indexByName.reserve(formals.size());
        for (std::size_t i = 0; i < formals.size(); ++i) {
            if (const auto *name = formalCallArgName(formals[i])) {
                indexByName.emplace(toStdString(*name), i);
            }
        }

        CallArgList ordered(formals.size());
        std::size_t positionalCount = 0;
        bool seenNamedArg = false;
        for (const auto &arg : normalized) {
            if (!isNamedCallArg(arg)) {
                if (seenNamedArg) {
                    error(
                        arg.syntax ? arg.syntax->loc : options.callLoc,
                        "positional arguments must come before named arguments",
                        "Write calls like `name(a, b, x=..., y=...)`, not "
                        "`name(x=..., a)`.");
                }
                if (positionalCount >= ordered.size()) {
                    error(options.callLoc,
                          "call argument count mismatch: expected at most " +
                              std::to_string(formals.size()) + ", got " +
                              std::to_string(normalized.size()));
                }
                ordered[positionalCount++] = arg;
                continue;
            }

            seenNamedArg = true;
            auto found = indexByName.find(*arg.name);
            if (found == indexByName.end()) {
                error(arg.syntax ? arg.syntax->loc : options.callLoc,
                      "unknown " + std::string(callBindingNameKind(options)) +
                          " `" + *arg.name + "` for " +
                          describeCallTarget(options),
                      formatAvailableFormalNames(formals, options));
            }
            if (ordered[found->second].value != nullptr) {
                error(arg.syntax ? arg.syntax->loc : options.callLoc,
                      "duplicate " + std::string(callBindingNameKind(options)) +
                          " `" + *arg.name + "` for " +
                          describeCallTarget(options),
                      "Each " + std::string(callBindingNameKind(options)) +
                          " can only be specified once.");
            }
            ordered[found->second] = arg;
        }

        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (!ordered[i].value) {
                auto *requiredName = formalCallArgName(formals[i]);
                error(options.callLoc,
                      "missing " + std::string(callBindingNameKind(options)) +
                          " `" +
                          (requiredName ? toStdString(*requiredName)
                                        : std::string()) +
                          "` for " + describeCallTarget(options),
                      formatAvailableFormalNames(formals, options));
            }
        }

        return ordered;
    }

    std::vector<BoundCallArg> bindCallArgs(
        const CallArgList &normalizedArgs,
        const std::vector<FormalCallArg> &formals,
        const CallBindingOptions &options) {
        auto orderedArgs =
            collectOrderedCallArgs(normalizedArgs, formals, options);
        if (orderedArgs.size() != formals.size()) {
            error(options.callLoc,
                  callBindingCountMismatchLabel(options) + ": expected " +
                      std::to_string(formals.size()) + ", got " +
                      std::to_string(orderedArgs.size()));
        }

        std::vector<BoundCallArg> boundArgs;
        boundArgs.reserve(orderedArgs.size());
        for (std::size_t i = 0; i < orderedArgs.size(); ++i) {
            const auto &spec = orderedArgs[i];
            const auto &formal = formals[i];
            const auto argLoc =
                spec.value ? spec.value->loc
                           : (spec.syntax ? spec.syntax->loc : options.callLoc);
            if (formal.bindingKind == BindingKind::Ref) {
                if (spec.bindingKind != BindingKind::Ref) {
                    error(spec.syntax ? spec.syntax->loc : options.callLoc,
                          "reference " + describeFormalCallArg(formal) +
                              " must be passed with `ref`",
                          formalCallArgMissingRefHint(formal));
                }
            } else if (spec.bindingKind == BindingKind::Ref) {
                error(spec.syntax ? spec.syntax->loc : options.callLoc,
                      disallowRefMessage(formal, options),
                      disallowRefHint(options));
            }

            auto *expr = requireNonCallExpr(spec.value, formal.type);
            if (formal.bindingKind == BindingKind::Ref) {
                if (!isAddressable(expr)) {
                    error(argLoc,
                          "reference " + describeFormalCallArg(formal) +
                              " expects an addressable value",
                          "Pass a variable, struct field, dereferenced "
                          "pointer, or array indexing expression.");
                }
                if (!canBindReferenceType(formal.type, expr->getType())) {
                    error(argLoc,
                          "reference " + describeFormalCallArg(formal) +
                              " type mismatch: expected " +
                              describeResolvedType(formal.type) + ", got " +
                              describeResolvedType(expr->getType()),
                          "Reference arguments can add const to the view, but "
                          "they cannot drop existing const qualifiers from the "
                          "referenced storage.");
                }
            } else {
                expr = coerceNumericExpr(expr, formal.type, argLoc, false);
                expr = coercePointerExpr(expr, formal.type, argLoc);
                requireCompatibleTypes(
                    argLoc, formal.type, expr->getType(),
                    formalCallArgTypeMismatchContext(formal));
            }

            boundArgs.push_back(
                {spec, expr, formal.type, formal.bindingKind, i});
        }
        return boundArgs;
    }

    std::vector<std::pair<string, TypeClass *>> orderedStructMembers(
        StructType *structType, const location &loc) {
        std::vector<std::pair<string, TypeClass *>> members(
            structType ? structType->getMembers().size() : 0);
        if (!structType) {
            return members;
        }
        for (const auto &entry : structType->getMembers()) {
            const auto index = static_cast<std::size_t>(entry.second.second);
            if (index >= members.size()) {
                internalError(loc,
                              "struct member index is out of range for `" +
                                  describeResolvedType(structType) + "`",
                              "This looks like a type layout bug.");
            }
            members[index] = {entry.first(), entry.second.first};
        }
        return members;
    }

    bool isAddressable(HIRExpr *expr) {
        if (!expr) {
            return false;
        }
        if (auto *value = dynamic_cast<HIRValue *>(expr)) {
            auto *object = value->getValue();
            return object && object->isVariable() && !object->isRegVal();
        }
        if (auto *selector = dynamic_cast<HIRSelector *>(expr)) {
            return selector->isValueFieldSelector() &&
                   isAddressable(selector->getParent());
        }
        if (auto *unary = dynamic_cast<HIRUnaryOper *>(expr)) {
            return unary->getOp() == '*' && unary->getType() != nullptr;
        }
        if (dynamic_cast<HIRIndex *>(expr)) {
            return true;
        }
        return false;
    }

    HIRBlock *analyzeBlock(AstNode *node) {
        auto *block = makeHIR<HIRBlock>(node ? node->loc : location());
        if (!node) {
            return block;
        }

        if (auto *list = node->as<AstStatList>()) {
            for (auto *stmt : list->getBody()) {
                auto *hirNode = analyzeStmt(stmt);
                if (hirNode) {
                    block->push(hirNode);
                }
            }
            return block;
        }

        auto *hirNode = analyzeStmt(node);
        if (hirNode) {
            block->push(hirNode);
        }
        return block;
    }

    HIRNode *analyzeStmt(AstNode *node) {
        if (!node) {
            return nullptr;
        }
        if (node->is<AstStatList>()) {
            return analyzeBlock(node);
        }
        if (auto *varDef = node->as<AstVarDef>()) {
            return analyzeVarDef(varDef);
        }
        if (auto *ret = node->as<AstRet>()) {
            return analyzeRet(ret);
        }
        if (auto *breakNode = node->as<AstBreak>()) {
            return analyzeBreak(breakNode);
        }
        if (auto *continueNode = node->as<AstContinue>()) {
            return analyzeContinue(continueNode);
        }
        if (auto *ifNode = node->as<AstIf>()) {
            return analyzeIf(ifNode);
        }
        if (auto *forNode = node->as<AstFor>()) {
            return analyzeFor(forNode);
        }
        if (node->is<AstStructDecl>() || node->is<AstTraitDecl>() ||
            node->is<AstTraitImplDecl>() || node->is<AstFuncDecl>() ||
            node->is<AstImport>()) {
            return nullptr;
        }
        auto *expr = requireNonCallExpr(node);
        return expr;
    }

    HIRExpr *analyzeExpr(AstNode *node, TypeClass *expectedType = nullptr) {
        if (!node) {
            return nullptr;
        }
        if (auto *constant = node->as<AstConst>()) {
            return analyzeConst(constant, expectedType);
        }
        if (auto *field = node->as<AstField>()) {
            return analyzeField(field);
        }
        if (auto *funcRef = node->as<AstFuncRef>()) {
            return analyzeFuncRef(funcRef);
        }
        if (auto *assign = node->as<AstAssign>()) {
            return analyzeAssign(assign);
        }
        if (auto *bin = node->as<AstBinOper>()) {
            return analyzeBinOper(bin, expectedType);
        }
        if (auto *unary = node->as<AstUnaryOper>()) {
            return analyzeUnaryOper(unary, expectedType);
        }
        if (auto *refExpr = node->as<AstRefExpr>()) {
            error(refExpr->loc, "`ref` is only valid as a call argument marker",
                  "Use it in calls like `f(ref x)` or `f(ref name = x)`.");
        }
        if (auto *tuple = node->as<AstTupleLiteral>()) {
            return analyzeTupleLiteral(tuple, expectedType);
        }
        if (auto *braceInit = node->as<AstBraceInit>()) {
            return analyzeBraceInit(braceInit, expectedType);
        }
        if (auto *dotLike = node->as<AstDotLike>()) {
            return analyzeDotLike(dotLike);
        }
        if (auto *typeApply = node->as<AstTypeApply>()) {
            if (resolvedGenericFunctionDecl(typeApply->value)) {
                diagnoseGenericFunctionValueUse(
                    describeGenericCallable(typeApply->value), typeApply->loc);
            }
            if (auto *binding = resolvedEntityBinding(typeApply->value);
                binding && binding->kind() == ResolvedEntityRef::Kind::GenericType) {
                diagnoseGenericTypeValueUse(
                    toStdString(binding->resolvedName()), typeApply->loc);
            }
            diagnoseGenericTypeApplyTarget(typeApply->loc);
        }
        if (auto *castExpr = node->as<AstCastExpr>()) {
            return analyzeCastExpr(castExpr);
        }
        if (auto *sizeofExpr = node->as<AstSizeofExpr>()) {
            return analyzeSizeofExpr(sizeofExpr);
        }
        if (auto *call = node->as<AstFieldCall>()) {
            return analyzeCall(call, expectedType);
        }
        error(node->loc, "unsupported AST node in HIR analysis");
    }

    HIRExpr *analyzeConst(AstConst *node, TypeClass *expectedType = nullptr) {
        return analyzeStaticLiteralInitializerExpr(typeMgr, ownerModule, node,
                                                   expectedType);
    }

    HIRExpr *analyzeTupleLiteral(AstTupleLiteral *node,
                                 TypeClass *expectedType) {
        auto *tupleType =
            expectedType ? expectedType->as<TupleType>() : nullptr;
        const auto actualCount = node->items ? node->items->size() : 0;

        std::vector<HIRExpr *> items;
        items.reserve(actualCount);

        if (!tupleType) {
            std::vector<TypeClass *> inferredItemTypes;
            inferredItemTypes.reserve(actualCount);
            for (size_t i = 0; i < actualCount; ++i) {
                auto *item = requireNonCallExpr(node->items->at(i));
                auto *itemType = item ? item->getType() : nullptr;
                if (!itemType) {
                    auto *value = dynamic_cast<HIRValue *>(item);
                    auto *object = value ? value->getValue() : nullptr;
                    if (object && object->as<TypeObject>()) {
                        error(node->items->at(i)->loc,
                              "type names can't be stored as tuple elements",
                              "Use the type in a type annotation, or construct "
                              "a runtime value from it.");
                    }
                    error(node->items->at(i)->loc,
                          "tuple element doesn't produce a storable runtime "
                          "value");
                }
                inferredItemTypes.push_back(itemType);
                items.push_back(item);
            }
            tupleType = typeMgr->getOrCreateTupleType(inferredItemTypes);
        } else {
            const auto &itemTypes = tupleType->getItemTypes();
            if (actualCount != itemTypes.size()) {
                error(node->loc, "tuple literal arity mismatch: expected " +
                                     std::to_string(itemTypes.size()) +
                                     " items, got " +
                                     std::to_string(actualCount));
            }

            for (size_t i = 0; i < actualCount; ++i) {
                auto *item =
                    requireNonCallExpr(node->items->at(i), itemTypes[i]);
                item = coerceNumericExpr(item, itemTypes[i],
                                         node->items->at(i)->loc, false);
                item = coercePointerExpr(item, itemTypes[i],
                                         node->items->at(i)->loc);
                requireCompatibleTypes(node->items->at(i)->loc, itemTypes[i],
                                       item->getType(),
                                       "tuple element type mismatch at index " +
                                           std::to_string(i));
                items.push_back(item);
            }
        }
        return makeHIR<HIRTupleLiteral>(std::move(items), tupleType, node->loc);
    }

    class InitialListLowering {
        FunctionAnalyzer &owner;

        struct InitialList;

        struct InitialListItem {
            location loc;
            AstNode *expr = nullptr;
            std::unique_ptr<InitialList> nested;
        };

        struct InitialList {
            location loc;
            std::vector<InitialListItem> items;
        };

        struct InferredArrayShape {
            TypeClass *elementType = nullptr;
            std::vector<std::size_t> extents;
        };

        TypeClass *mergeInferredElementType(TypeClass *current, TypeClass *next,
                                            const location &loc) {
            if (!current) {
                return next;
            }
            if (!next || current == next) {
                return current;
            }
            if (auto *common =
                    commonNumericType(owner.typeMgr, current, next)) {
                return common;
            }
            if (canImplicitPointerViewConversion(current, next)) {
                return current;
            }
            if (canImplicitPointerViewConversion(next, current)) {
                return next;
            }
            owner.error(
                loc,
                "cannot infer a common array element type from `" +
                    describeResolvedType(current) + "` and `" +
                    describeResolvedType(next) + "`",
                numericConversionHint() + " " + pointerConversionHint());
        }

        ArrayType *buildArrayTypeFromShape(const InferredArrayShape &shape,
                                           const location &loc) {
            if (!shape.elementType || shape.extents.empty()) {
                return nullptr;
            }
            TypeClass *current = shape.elementType;
            for (auto it = shape.extents.rbegin(); it != shape.extents.rend();
                 ++it) {
                std::vector<AstNode *> dims;
                dims.push_back(owner.makeStaticDimensionNode(*it, loc));
                current =
                    owner.typeMgr->createArrayType(current, std::move(dims));
            }
            return asUnqualified<ArrayType>(current);
        }

        InferredArrayShape inferArrayShape(const InitialList &initList) {
            if (initList.items.empty()) {
                owner.error(
                    initList.loc,
                    "cannot infer an array type from an empty brace "
                    "initializer",
                    "Write an explicit type like `var a i32[2] = {}`, or "
                    "provide at least one element such as `var a = {1, 2}`.");
            }

            InferredArrayShape shape;
            shape.extents.push_back(initList.items.size());

            const bool hasNested =
                std::any_of(initList.items.begin(), initList.items.end(),
                            [](const InitialListItem &item) {
                                return item.nested != nullptr;
                            });

            if (!hasNested) {
                for (const auto &item : initList.items) {
                    auto *expr = owner.requireNonCallExpr(item.expr);
                    if (!expr->getType()) {
                        owner.error(
                            item.loc,
                            "array element does not produce an inferable "
                            "runtime type",
                            "Write an explicit array type, or use elements "
                            "with concrete runtime value types.");
                    }
                    shape.elementType = mergeInferredElementType(
                        shape.elementType, expr->getType(), item.loc);
                }
                return shape;
            }

            std::optional<std::vector<std::size_t>> childExtents;
            for (const auto &item : initList.items) {
                if (!item.nested) {
                    owner.error(item.loc,
                                "array initializer cannot mix nested and "
                                "non-nested elements",
                                "Use either `{1, 2}` for a flat array, or "
                                "`{{1, 2}, {3, 4}}` for a nested array.");
                }
                auto childShape = inferArrayShape(*item.nested);
                if (!childExtents) {
                    childExtents = childShape.extents;
                } else if (*childExtents != childShape.extents) {
                    owner.error(item.loc,
                                "nested array initializer rows must have a "
                                "consistent shape",
                                "Keep each nested brace group the same length, "
                                "for example `{{1, 2}, {3, 4}}`.");
                }
                shape.elementType = mergeInferredElementType(
                    shape.elementType, childShape.elementType, item.loc);
            }
            if (childExtents) {
                shape.extents.insert(shape.extents.end(), childExtents->begin(),
                                     childExtents->end());
            }
            return shape;
        }

        std::vector<AstNode *> consumeArrayOuterDimension(
            const std::vector<AstNode *> &dims) {
            std::vector<AstNode *> remaining;
            remaining.reserve(dims.size());
            bool consumed = false;
            const bool legacyPrefix = isLegacyArrayDimensionPrefix(dims);
            for (auto *dim : dims) {
                if (dim == nullptr) {
                    continue;
                }
                if (!consumed) {
                    consumed = true;
                    continue;
                }
                remaining.push_back(dim);
            }
            if (legacyPrefix && remaining.size() > 1) {
                remaining.insert(remaining.begin(), nullptr);
            }
            return remaining;
        }

        TypeClass *arrayInitChildType(ArrayType *arrayType) {
            if (!arrayType) {
                return nullptr;
            }
            bool ok = false;
            auto dims = arrayType->staticDimensions(&ok);
            if (!ok || dims.empty()) {
                return nullptr;
            }
            if (dims.size() == 1) {
                return arrayType->getElementType();
            }
            auto childDims =
                consumeArrayOuterDimension(arrayType->getDimensions());
            return owner.typeMgr->createArrayType(arrayType->getElementType(),
                                                  std::move(childDims));
        }

        std::unique_ptr<InitialList> buildInitialList(AstBraceInit *node) {
            if (!node) {
                return nullptr;
            }
            auto initList = std::make_unique<InitialList>();
            initList->loc = node->loc;
            if (!node->items || node->items->empty()) {
                return initList;
            }
            initList->items.reserve(node->items->size());
            for (auto *rawItem : *node->items) {
                auto *braceItem = dynamic_cast<AstBraceInitItem *>(rawItem);
                if (!braceItem || !braceItem->value) {
                    owner.error(node->loc,
                                "array initializer contains an invalid item",
                                "Each item must be an expression or a nested "
                                "brace group.");
                }
                InitialListItem item;
                item.loc = braceItem->value->loc;
                if (auto *nested =
                        dynamic_cast<AstBraceInit *>(braceItem->value)) {
                    item.nested = buildInitialList(nested);
                } else {
                    item.expr = braceItem->value;
                }
                initList->items.push_back(std::move(item));
            }
            return initList;
        }

        HIRExpr *materializeArrayInit(const InitialList &initList,
                                      ArrayType *arrayType,
                                      const location &loc) {
            if (!arrayType) {
                owner.internalError(loc,
                                    "invalid array initializer materialization",
                                    "This looks like a compiler pipeline bug.");
            }
            if (!arrayType->hasStaticLayout()) {
                owner.error(
                    loc,
                    "array initializers currently require fixed explicit "
                    "dimensions",
                    "Use positive integer literal dimensions. Dimension "
                    "inference and dynamic sizes are not implemented yet.");
            }

            bool ok = false;
            auto dims = arrayType->staticDimensions(&ok);
            if (!ok || dims.empty()) {
                owner.error(
                    loc,
                    "array initializers currently require fixed explicit "
                    "dimensions",
                    "Use positive integer literal dimensions. Dimension "
                    "inference and dynamic sizes are not implemented yet.");
            }
            const auto outerExtent = static_cast<std::size_t>(dims.front());
            auto *childType = arrayInitChildType(arrayType);
            if (!childType) {
                owner.error(loc,
                            "array initializer is missing its element type",
                            "This looks like a compiler pipeline bug.");
            }

            std::vector<HIRExpr *> items;
            if (initList.items.size() > outerExtent) {
                owner.error(
                    loc,
                    "array initializer has too many elements: expected at "
                    "most " +
                        std::to_string(outerExtent) + ", got " +
                        std::to_string(initList.items.size()),
                    "Remove extra elements or increase the array dimension.");
            }
            if (isFixedArrayOfFunctionPointerValues(arrayType) &&
                initList.items.size() != outerExtent) {
                owner.error(loc,
                            "function pointer arrays require full "
                            "initialization: expected exactly " +
                                std::to_string(outerExtent) +
                                " elements, got " +
                                std::to_string(initList.items.size()),
                            "Initialize every slot explicitly. Missing "
                            "elements would become null function pointers.");
            }

            items.reserve(initList.items.size());
            for (std::size_t i = 0; i < initList.items.size(); ++i) {
                const auto &item = initList.items[i];
                if (item.nested) {
                    auto *childArrayType = asUnqualified<ArrayType>(childType);
                    if (!childArrayType) {
                        owner.error(item.loc,
                                    "array initializer nesting is deeper than "
                                    "the array shape",
                                    "Remove this brace level, or make the "
                                    "target element type another array.");
                    }
                    items.push_back(materializeArrayInit(
                        *item.nested, childArrayType, item.loc));
                    continue;
                }

                if (asUnqualified<ArrayType>(childType)) {
                    owner.error(item.loc,
                                "array initializer expects a nested brace "
                                "group at index " +
                                    std::to_string(i),
                                "Write nested rows like `{{1, 2}, {3, 4}}` so "
                                "the brace structure matches the array shape.");
                }

                auto *value = owner.requireNonCallExpr(item.expr, childType);
                value =
                    owner.coerceNumericExpr(value, childType, item.loc, false);
                value = owner.coercePointerExpr(value, childType, item.loc);
                owner.requireCompatibleTypes(
                    item.loc, childType, value->getType(),
                    "array initializer element type mismatch at index " +
                        std::to_string(i));
                items.push_back(value);
            }

            return owner.makeHIR<HIRArrayInit>(std::move(items), arrayType,
                                               loc);
        }

        HIRExpr *materializeInitList(const InitialList &initList,
                                     TypeClass *expectedType,
                                     const location &loc) {
            if (!expectedType) {
                auto inferredShape = inferArrayShape(initList);
                expectedType = buildArrayTypeFromShape(inferredShape, loc);
                if (!expectedType) {
                    owner.internalError(
                        loc, "failed to build inferred array type",
                        "This looks like a compiler pipeline bug.");
                }
            }
            if (auto *arrayType = asUnqualified<ArrayType>(expectedType)) {
                return materializeArrayInit(initList, arrayType, loc);
            }
            if (auto *structType = asUnqualified<StructType>(expectedType)) {
                owner.error(
                    loc,
                    "brace initialization currently applies to arrays only",
                    "For structs, call the type directly like `" +
                        describeResolvedType(structType) +
                        "(...)` using positional or named arguments. "
                        "`initial_list` remains an internal "
                        "array-initialization interface.");
            }
            owner.error(loc,
                        "brace initializer currently supports arrays only when "
                        "the target type is already known",
                        "Write a declaration like `var matrix i32[4][5] = "
                        "{{1}, {2}}`, or call a struct type like `Vec2(x=1, "
                        "y=2)` for struct construction.");
        }

    public:
        explicit InitialListLowering(FunctionAnalyzer &owner) : owner(owner) {}

        HIRExpr *analyze(AstBraceInit *node, TypeClass *expectedType) {
            auto initList = buildInitialList(node);
            return materializeInitList(*initList, expectedType, node->loc);
        }
    };

    HIRExpr *analyzeBraceInit(AstBraceInit *node, TypeClass *expectedType) {
        return InitialListLowering(*this).analyze(node, expectedType);
    }

    HIRExpr *analyzeField(AstField *node) {
        auto *binding = resolved.field(node);
        if (binding && binding->kind() == ResolvedEntityRef::Kind::Module) {
            diagnoseModuleNamespaceValueUse(toStdString(node->name), node->loc);
        }
        if (binding && binding->kind() == ResolvedEntityRef::Kind::Trait) {
            diagnoseTraitNamespaceValueUse(toStdString(binding->resolvedName()),
                                           node->loc);
        }
        return materializeResolvedEntity(binding, node->loc,
                                         toStdString(node->name));
    }

    HIRExpr *analyzeResolvedDotLike(AstDotLike *node) {
        auto *binding = resolved.dotLike(node);
        if (!binding || !binding->valid()) {
            return nullptr;
        }
        if (binding->kind() == ResolvedEntityRef::Kind::Trait) {
            diagnoseTraitNamespaceValueUse(toStdString(binding->resolvedName()),
                                           node->loc);
        }
        return materializeResolvedEntity(binding, node->loc,
                                         describeMemberOwnerSyntax(node));
    }

    const ModuleInterface::TypeDecl *
    resolveVisibleTypeDecl(BaseTypeNode *base,
                           const ModuleInterface *ownerInterface = nullptr) const {
        if (!base || !unit) {
            return nullptr;
        }

        auto rawName = baseTypeName(base);
        std::string moduleName;
        std::string memberName;
        if (!splitBaseTypeName(base, moduleName, memberName)) {
            if (ownerInterface) {
                auto ownerLookup = ownerInterface->lookupTopLevelName(rawName);
                if (ownerLookup.isType()) {
                    return ownerLookup.typeDecl;
                }
            }
            auto lookup = unit->lookupTopLevelName(rawName);
            return lookup.isType() ? lookup.typeDecl : nullptr;
        }

        if (ownerInterface) {
            if (moduleName == ownerInterface->moduleName() ||
                moduleName == toStdString(ownerInterface->exportNamespacePrefix())) {
                auto ownerLookup = ownerInterface->lookupTopLevelName(memberName);
                if (ownerLookup.isType()) {
                    return ownerLookup.typeDecl;
                }
            }
            if (const auto *ownerImported =
                    ownerInterface->findImportedModule(moduleName);
                ownerImported && ownerImported->interface) {
                auto ownerLookup =
                    ownerImported->interface->lookupTopLevelName(memberName);
                if (ownerLookup.isType()) {
                    return ownerLookup.typeDecl;
                }
            }
        }

        const auto *imported = unit->findImportedModule(moduleName);
        if (!imported || !imported->interface) {
            return nullptr;
        }
        auto lookup = unit->lookupTopLevelName(*imported, memberName);
        return lookup.isType() ? lookup.typeDecl : nullptr;
    }

    std::string buildAppliedTypeName(const std::string &baseName,
                                     const std::vector<TypeClass *> &args) {
        std::string name = baseName.empty() ? std::string("<type>")
                                            : baseName;
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

    std::string appliedTypeDisplayName(BaseTypeNode *base,
                                       const ModuleInterface::TypeDecl *typeDecl,
                                       const ModuleInterface *ownerInterface) const {
        if (!base) {
            return typeDecl ? toStdString(typeDecl->exportedName)
                            : std::string("<type>");
        }

        std::string moduleName;
        std::string memberName;
        if (splitBaseTypeName(base, moduleName, memberName)) {
            return baseTypeName(base);
        }

        auto rawName = baseTypeName(base);
        if (ownerInterface) {
            auto ownerLookup = ownerInterface->lookupTopLevelName(rawName);
            if (ownerLookup.isType() && ownerLookup.typeDecl) {
                return toStdString(ownerLookup.typeDecl->exportedName);
            }
        }

        auto localLookup = unit->lookupTopLevelName(rawName);
        if (localLookup.isType()) {
            return rawName;
        }
        return typeDecl ? toStdString(typeDecl->exportedName) : rawName;
    }

    const CompilationUnit *ownerContextUnit(
        const ModuleInterface *ownerInterface) const {
        if (!unit) {
            return nullptr;
        }
        return unit->contextUnitForInterface(ownerInterface);
    }

    StructType *materializeVisibleAppliedStructType(
        const ModuleInterface::TypeDecl &typeDecl,
        const std::vector<TypeClass *> &argTypes, const location &loc,
        const ModuleInterface *ownerInterface) {
        if (!unit) {
            return nullptr;
        }
        auto *contextUnit = ownerContextUnit(ownerInterface);
        if (!contextUnit) {
            return nullptr;
        }
        auto *structType = unit->materializeAppliedStructType(
            typeMgr, typeDecl, std::vector<TypeClass *>(argTypes),
            *contextUnit);
        if (!structType) {
            internalError(
                loc,
                "generic applied type `" +
                    toStdString(typeDecl.exportedName) +
                    "` did not materialize a concrete runtime type",
                "This looks like a generic struct instantiation bug.");
        }
        return structType;
    }

    TypeClass *substituteGenericSignatureType(
        TypeNode *node, const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const location &loc, const std::string &functionName,
        const ModuleInterface *ownerInterface) {
        if (!node) {
            return nullptr;
        }

        if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
            return substituteGenericSignatureType(param->type, genericArgs, loc,
                                                 functionName, ownerInterface);
        }
        if (dynamic_cast<AnyTypeNode *>(node)) {
            return typeMgr->createAnyType();
        }
        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            auto rawName = baseTypeName(base);
            if (auto found = genericArgs.find(rawName); found != genericArgs.end()) {
                return found->second;
            }
            if (auto *typeDecl = resolveVisibleTypeDecl(base, ownerInterface)) {
                if (typeDecl->type) {
                    return typeDecl->type;
                }
            }
            auto *type =
                unit ? unit->resolveType(typeMgr, node) : typeMgr->getType(node);
            if (!type) {
                error(loc,
                      "generic function `" + functionName +
                          "` uses unsupported signature type `" + rawName +
                          "` before instantiation",
                      "This signature still depends on generic substitution "
                      "or an unsupported template form.");
            }
            return type;
        }
        if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
            auto *base = dynamic_cast<BaseTypeNode *>(applied->base);
            auto *typeDecl = resolveVisibleTypeDecl(base, ownerInterface);
            if (!typeDecl) {
                error(loc,
                      "generic function `" + functionName +
                          "` uses unsupported applied signature type `" +
                          describeTypeNode(applied, "<unknown type>") +
                          "` before instantiation");
            }
            if (!typeDecl->isGeneric()) {
                error(loc,
                      "generic function `" + functionName +
                          "` applies `[...]` arguments to non-generic type `" +
                          toStdString(typeDecl->exportedName) + "`");
            }
            if (applied->args.size() != typeDecl->typeParams.size()) {
                error(loc,
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
                argTypes.push_back(substituteGenericSignatureType(
                    arg, genericArgs, loc, functionName, ownerInterface));
            }
            if (unit) {
                if (auto *structType = materializeVisibleAppliedStructType(
                        *typeDecl, argTypes, loc, ownerInterface)) {
                    return structType;
                }
            }
            return typeMgr->createOpaqueStructType(
                buildAppliedTypeName(
                    appliedTypeDisplayName(base, typeDecl, ownerInterface),
                    argTypes),
                typeDecl->declKind, typeDecl->exportedName, argTypes);
        }
        if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
            auto *baseType = substituteGenericSignatureType(
                qualified->base, genericArgs, loc, functionName,
                ownerInterface);
            return baseType ? typeMgr->createConstType(baseType) : nullptr;
        }
        if (auto *dynType = dynamic_cast<DynTypeNode *>(node)) {
            auto *type =
                unit ? unit->resolveType(typeMgr, dynType) : typeMgr->getType(node);
            if (!type) {
                error(loc,
                      "generic function `" + functionName +
                          "` uses unsupported dyn signature type `" +
                          describeTypeNode(node, "<unknown type>") +
                          "` before instantiation");
            }
            return type;
        }
        if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
            auto *baseType = substituteGenericSignatureType(
                pointer->base, genericArgs, loc, functionName, ownerInterface);
            for (uint32_t i = 0; baseType && i < pointer->dim; ++i) {
                baseType = typeMgr->createPointerType(baseType);
            }
            return baseType;
        }
        if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
            auto *elementType = substituteGenericSignatureType(
                indexable->base, genericArgs, loc, functionName,
                ownerInterface);
            return elementType ? typeMgr->createIndexablePointerType(elementType)
                               : nullptr;
        }
        if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
            auto *elementType = substituteGenericSignatureType(
                array->base, genericArgs, loc, functionName, ownerInterface);
            return elementType ? typeMgr->createArrayType(elementType, array->dim)
                               : nullptr;
        }
        if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
            std::vector<TypeClass *> itemTypes;
            itemTypes.reserve(tuple->items.size());
            for (auto *item : tuple->items) {
                itemTypes.push_back(substituteGenericSignatureType(
                    item, genericArgs, loc, functionName, ownerInterface));
            }
            return typeMgr->getOrCreateTupleType(itemTypes);
        }
        if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
            std::vector<TypeClass *> argTypes;
            std::vector<BindingKind> argBindingKinds;
            argTypes.reserve(func->args.size());
            argBindingKinds.reserve(func->args.size());
            for (auto *arg : func->args) {
                argBindingKinds.push_back(funcParamBindingKind(arg));
                argTypes.push_back(substituteGenericSignatureType(
                    unwrapFuncParamType(arg), genericArgs, loc, functionName,
                    ownerInterface));
            }
            auto *retType = substituteGenericSignatureType(
                func->ret, genericArgs, loc, functionName, ownerInterface);
            auto *funcType = typeMgr->getOrCreateFunctionType(
                argTypes, retType, std::move(argBindingKinds));
            return funcType ? typeMgr->createPointerType(funcType) : nullptr;
        }

        error(loc,
              "generic function `" + functionName +
                  "` uses unsupported signature type `" +
                  describeTypeNode(node, "<unknown type>") +
                  "` before instantiation");
    }

    void inferGenericArgsFromPattern(
        TypeNode *pattern, TypeClass *actualType,
        std::unordered_map<std::string, TypeClass *> &selectedByName,
        const location &loc, const std::string &functionName,
        const ModuleInterface *ownerInterface) {
        if (!pattern || !actualType) {
            return;
        }
        if (auto *param = dynamic_cast<FuncParamTypeNode *>(pattern)) {
            inferGenericArgsFromPattern(param->type, actualType, selectedByName,
                                        loc, functionName, ownerInterface);
            return;
        }
        if (auto *base = dynamic_cast<BaseTypeNode *>(pattern)) {
            auto rawName = baseTypeName(base);
            auto found = selectedByName.find(rawName);
            if (found == selectedByName.end()) {
                return;
            }
            if (!found->second) {
                found->second = actualType;
                return;
            }
            if (found->second != actualType) {
                error(loc,
                      "cannot infer a single concrete type for `" + rawName +
                          "` in `" + functionName + "`",
                      "Pass explicit type arguments like `" + functionName +
                          "[T](...)` when inference would need to choose "
                          "between different concrete argument types.");
            }
            return;
        }
        if (auto *qualified = dynamic_cast<ConstTypeNode *>(pattern)) {
            if (auto *actualConst = actualType->as<ConstType>()) {
                inferGenericArgsFromPattern(qualified->base,
                                            actualConst->getBaseType(),
                                            selectedByName, loc, functionName,
                                            ownerInterface);
            } else {
                inferGenericArgsFromPattern(qualified->base, actualType,
                                            selectedByName, loc, functionName,
                                            ownerInterface);
            }
            return;
        }
        if (auto *pointer = dynamic_cast<PointerTypeNode *>(pattern)) {
            auto *current = actualType;
            for (uint32_t i = 0; i < pointer->dim; ++i) {
                auto *pointerType = asUnqualified<PointerType>(current);
                if (!pointerType) {
                    return;
                }
                current = pointerType->getPointeeType();
            }
            inferGenericArgsFromPattern(pointer->base, current, selectedByName,
                                        loc, functionName, ownerInterface);
            return;
        }
        if (auto *indexable =
                dynamic_cast<IndexablePointerTypeNode *>(pattern)) {
            auto *indexableType = asUnqualified<IndexablePointerType>(actualType);
            if (!indexableType) {
                return;
            }
            inferGenericArgsFromPattern(indexable->base,
                                        indexableType->getElementType(),
                                        selectedByName, loc, functionName,
                                        ownerInterface);
            return;
        }
        if (auto *array = dynamic_cast<ArrayTypeNode *>(pattern)) {
            auto *arrayType = asUnqualified<ArrayType>(actualType);
            if (!arrayType) {
                return;
            }
            inferGenericArgsFromPattern(array->base, arrayType->getElementType(),
                                        selectedByName, loc, functionName,
                                        ownerInterface);
            return;
        }
        if (auto *tuple = dynamic_cast<TupleTypeNode *>(pattern)) {
            auto *tupleType = asUnqualified<TupleType>(actualType);
            if (!tupleType ||
                tupleType->getItemTypes().size() != tuple->items.size()) {
                return;
            }
            for (std::size_t i = 0; i < tuple->items.size(); ++i) {
                inferGenericArgsFromPattern(tuple->items[i],
                                            tupleType->getItemTypes()[i],
                                            selectedByName, loc, functionName,
                                            ownerInterface);
            }
            return;
        }
        if (auto *func = dynamic_cast<FuncPtrTypeNode *>(pattern)) {
            auto *pointerType = asUnqualified<PointerType>(actualType);
            auto *funcType =
                pointerType ? pointerType->getPointeeType()->as<FuncType>()
                            : nullptr;
            if (!funcType ||
                funcType->getArgTypes().size() != func->args.size()) {
                return;
            }
            for (std::size_t i = 0; i < func->args.size(); ++i) {
                inferGenericArgsFromPattern(unwrapFuncParamType(func->args[i]),
                                            funcType->getArgTypes()[i],
                                            selectedByName, loc, functionName,
                                            ownerInterface);
            }
            inferGenericArgsFromPattern(func->ret, funcType->getRetType(),
                                        selectedByName, loc, functionName,
                                        ownerInterface);
            return;
        }
        if (auto *applied = dynamic_cast<AppliedTypeNode *>(pattern)) {
            auto *base = dynamic_cast<BaseTypeNode *>(applied->base);
            auto *typeDecl = resolveVisibleTypeDecl(base, ownerInterface);
            auto *actualStruct = asUnqualified<StructType>(actualType);
            if (!typeDecl || !actualStruct ||
                !actualStruct->isAppliedTemplateInstance()) {
                return;
            }
            if (actualStruct->getAppliedTemplateName() !=
                typeDecl->exportedName) {
                return;
            }
            const auto &actualArgs = actualStruct->getAppliedTypeArgs();
            if (actualArgs.size() != applied->args.size()) {
                return;
            }
            for (std::size_t i = 0; i < applied->args.size(); ++i) {
                inferGenericArgsFromPattern(applied->args[i], actualArgs[i],
                                            selectedByName, loc, functionName,
                                            ownerInterface);
            }
            return;
        }
    }

    std::unordered_map<std::string, TypeClass *> resolveGenericCallTypeArgs(
        const ModuleInterface::FunctionDecl &functionDecl,
        const CallArgList &normalizedArgs,
        std::vector<TypeNode *> *explicitTypeArgs, const location &loc,
        const std::string &functionName,
        const ModuleInterface *ownerInterface) {
        const auto paramCount = functionDecl.paramTypeNodes.size();
        if (functionDecl.paramBindingKinds.size() != paramCount) {
            internalError(loc,
                          "generic function `" + functionName +
                              "` is missing parameter binding metadata",
                          "Rebuild interface collection before analyzing "
                          "generic calls.");
        }

        std::vector<FormalCallArg> syntaxFormals;
        syntaxFormals.reserve(paramCount);
        for (std::size_t i = 0; i < paramCount; ++i) {
            const string *paramName =
                i < functionDecl.paramNames.size()
                    ? &functionDecl.paramNames[i]
                    : nullptr;
            syntaxFormals.push_back({paramName, nullptr,
                                     functionDecl.paramBindingKinds[i],
                                     FormalCallArgKind::FunctionParameter, i});
        }
        auto orderedArgs = collectOrderedCallArgs(
            normalizedArgs, syntaxFormals,
            {loc, CallBindingTargetKind::FunctionCall, nullptr,
             !functionDecl.paramNames.empty()});

        std::unordered_map<std::string, std::size_t> genericIndexByName;
        std::unordered_map<std::string, TypeClass *> selectedByName;
        genericIndexByName.reserve(functionDecl.typeParams.size());
        selectedByName.reserve(functionDecl.typeParams.size());
        for (std::size_t i = 0; i < functionDecl.typeParams.size(); ++i) {
            auto name = toStdString(functionDecl.typeParams[i].localName);
            genericIndexByName.emplace(name, i);
            selectedByName.emplace(name, nullptr);
        }

        std::vector<TypeClass *> selected(functionDecl.typeParams.size(),
                                          nullptr);
        if (explicitTypeArgs && !explicitTypeArgs->empty()) {
            if (explicitTypeArgs->size() != functionDecl.typeParams.size()) {
                error(loc,
                      "generic type argument count mismatch for `" +
                          functionName + "`: expected " +
                          std::to_string(functionDecl.typeParams.size()) +
                          ", got " + std::to_string(explicitTypeArgs->size()),
                    "Match the number of `[` `]` type arguments to the "
                      "generic parameter list.");
            }
            for (std::size_t i = 0; i < explicitTypeArgs->size(); ++i) {
                auto *type = requireType(
                    explicitTypeArgs->at(i), explicitTypeArgs->at(i)->loc,
                    "unknown generic type argument at index " +
                        std::to_string(i) + " for `" + functionName + "`");
                selected[i] = type;
                selectedByName[toStdString(functionDecl.typeParams[i].localName)] =
                    type;
            }
        } else if (!functionDecl.typeParams.empty()) {
            for (std::size_t i = 0; i < paramCount; ++i) {
                auto *expr = requireNonCallExpr(orderedArgs[i].value);
                auto *actualType = expr ? expr->getType() : nullptr;
                if (!actualType) {
                    error(orderedArgs[i].loc,
                          "cannot infer generic type argument from a "
                          "non-value expression in `" +
                              functionName + "`");
                }
                inferGenericArgsFromPattern(functionDecl.paramTypeNodes[i],
                                            actualType, selectedByName,
                                            orderedArgs[i].loc, functionName,
                                            ownerInterface);
            }
            for (std::size_t i = 0; i < selected.size(); ++i) {
                auto *inferred =
                    selectedByName[toStdString(functionDecl.typeParams[i].localName)];
                selected[i] = inferred;
                if (!inferred) {
                    error(loc,
                          "cannot infer generic type argument `" +
                              toStdString(
                                  functionDecl.typeParams[i].localName) +
                              "` for `" + functionName + "`",
                          "Pass explicit type arguments like `" + functionName +
                              "[T](...)`.");
                }
            }
        }

        std::unordered_map<std::string, TypeClass *> genericArgs;
        genericArgs.reserve(functionDecl.typeParams.size());
        for (std::size_t i = 0; i < functionDecl.typeParams.size(); ++i) {
            genericArgs.emplace(
                toStdString(functionDecl.typeParams[i].localName), selected[i]);
        }
        return genericArgs;
    }

    void enforceGenericTraitBounds(
        const std::vector<ModuleInterface::GenericParamDecl> &typeParams,
        const std::unordered_map<std::string, TypeClass *> &genericArgs,
        const location &loc, const std::string &context) {
        if (!unit) {
            return;
        }
        for (const auto &param : typeParams) {
            if (param.boundTraitName.empty()) {
                continue;
            }
            auto found = genericArgs.find(toStdString(param.localName));
            if (found == genericArgs.end() || !found->second) {
                internalError(
                    loc,
                    context + " is missing a concrete type for bound parameter `" +
                        toStdString(param.localName) + "`",
                    "This looks like a generic bound selection bug.");
            }
            auto visibleImpls =
                unit->findVisibleTraitImpls(param.boundTraitName, found->second);
            if (!visibleImpls.empty()) {
                continue;
            }
            error(loc,
                  "type `" + describeResolvedType(found->second) +
                      "` does not satisfy bound `" +
                      toStdString(param.boundTraitName) +
                      "` for generic parameter `" +
                      toStdString(param.localName) + "` in " + context,
                  "Add `impl " + toStdString(param.boundTraitName) +
                      " for " + describeResolvedType(found->second) +
                      " { ... }` in a visible module, or choose a type that already "
                      "satisfies the bound.");
        }
    }

    HIRExpr *analyzeGenericFunctionCall(
        AstFieldCall *node, const ModuleInterface::FunctionDecl *functionDecl,
        std::vector<TypeNode *> *explicitTypeArgs,
        const std::string &functionName,
        const ModuleInterface *ownerInterface) {
        if (!functionDecl || !functionDecl->isGeneric()) {
            internalError(node ? node->loc : location(),
                          "generic call lowering is missing its template "
                          "signature",
                          "Run interface collection before analyzing "
                          "generic calls.");
        }

        auto normalizedArgs = normalizeCallArgs(node->args, node->loc);
        auto genericArgs = resolveGenericCallTypeArgs(
            *functionDecl, normalizedArgs, explicitTypeArgs, node->loc,
            functionName, ownerInterface);
        enforceGenericTraitBounds(functionDecl->typeParams, genericArgs,
                                  node->loc,
                                  "generic function `" + functionName + "`");

        if (ownerContextUnit(ownerInterface)) {
            auto *func = instantiateGenericFunction(
                *functionDecl, genericArgs, node->loc, ownerInterface);
            return lowerResolvedCall(makeHIR<HIRValue>(func, node->loc),
                                     std::move(normalizedArgs), node->loc,
                                     true);
        }

        std::vector<FormalCallArg> typedFormals;
        typedFormals.reserve(functionDecl->paramTypeNodes.size());
        for (std::size_t i = 0; i < functionDecl->paramTypeNodes.size();
             ++i) {
            const string *paramName =
                i < functionDecl->paramNames.size()
                    ? &functionDecl->paramNames[i]
                    : nullptr;
            auto *paramType = substituteGenericSignatureType(
                functionDecl->paramTypeNodes[i], genericArgs, node->loc,
                functionName, ownerInterface);
            typedFormals.push_back({paramName, paramType,
                                    functionDecl->paramBindingKinds[i],
                                    FormalCallArgKind::FunctionParameter, i});
        }
        (void)bindCallArgs(normalizedArgs, typedFormals,
                           {node->loc, CallBindingTargetKind::FunctionCall,
                            nullptr, !functionDecl->paramNames.empty()});
        diagnoseGenericInstantiationPending(functionName, node->loc);
    }

    HIRExpr *tryAnalyzeGenericMethodCall(
        AstFieldCall *node, AstDotLike *calleeSyntax,
        std::vector<TypeNode *> *explicitTypeArgs = nullptr) {
        if (!node || !calleeSyntax) {
            return nullptr;
        }
        if (resolvedTraitBinding(calleeSyntax->parent)) {
            return nullptr;
        }
        if (auto *binding = resolvedEntityBinding(calleeSyntax->parent);
            binding && binding->kind() == ResolvedEntityRef::Kind::Module) {
            return nullptr;
        }

        auto normalizedArgs = normalizeCallArgs(node->args, node->loc);
        auto *receiver = requireExpr(calleeSyntax->parent);
        auto methodName = toStdString(calleeSyntax->field->text);
        auto receiverAttempt = lookupMemberWithImplicitDeref(
            receiver, methodName, calleeSyntax->loc,
            !isExplicitDerefSyntax(calleeSyntax->parent));
        auto *receiverStructType =
            receiverAttempt.parent
                ? asUnqualified<StructType>(receiverAttempt.parent->getType())
                : nullptr;
        if (!receiverStructType) {
            return nullptr;
        }

        auto lookup = lookupGenericMethodTemplate(
            receiverStructType, toStringRef(methodName), calleeSyntax->loc);
        if (!lookup.found()) {
            return nullptr;
        }

        auto *ownerInterface =
            lookup.ownerUnit != unit ? lookup.ownerUnit->interface() : nullptr;
        auto genericArgs = resolveGenericMethodTypeArgs(
            *lookup.typeDecl, *lookup.methodTemplate, receiverStructType,
            normalizedArgs, explicitTypeArgs, node->loc,
            describeMemberOwnerSyntax(calleeSyntax), ownerInterface);
        enforceGenericTraitBounds(
            lookup.methodTemplate->typeParams, genericArgs, node->loc,
            "generic method `" + describeMemberOwnerSyntax(calleeSyntax) + "`");

        std::vector<TypeClass *> methodTypeArgs;
        for (std::size_t i = lookup.methodTemplate->enclosingTypeParamCount;
             i < lookup.methodTemplate->typeParams.size(); ++i) {
            auto found = genericArgs.find(
                toStdString(lookup.methodTemplate->typeParams[i].localName));
            if (found == genericArgs.end() || !found->second) {
                internalError(
                    node->loc,
                    "generic method `" + describeMemberOwnerSyntax(calleeSyntax) +
                        "` is missing a concrete type argument",
                    "This looks like a generic method inference bug.");
            }
            methodTypeArgs.push_back(found->second);
        }

        auto *methodFunc = instantiateGenericMethod(
            receiverStructType, lookup, genericArgs, methodTypeArgs, node->loc);
        return lowerDirectGenericMethodCall(
            methodFunc, receiverAttempt.parent, std::move(normalizedArgs),
            node->loc, toStringRef(lookup.methodTemplate->localName));
    }

    HIRExpr *analyzeGenericTypeCall(
        AstFieldCall *node, const ModuleInterface::TypeDecl *typeDecl,
        std::vector<TypeNode *> *explicitTypeArgs, const std::string &typeName,
        const ModuleInterface *ownerInterface) {
        if (!typeDecl || !typeDecl->isGeneric()) {
            internalError(node ? node->loc : location(),
                          "generic type call lowering is missing its template "
                          "signature",
                          "Run interface collection before analyzing "
                          "generic type constructors.");
        }
        if (!explicitTypeArgs || explicitTypeArgs->empty()) {
            diagnoseGenericTypeCall(typeName, node->loc);
        }
        if (explicitTypeArgs->size() != typeDecl->typeParams.size()) {
            error(node->loc,
                  "generic type argument count mismatch for `" + typeName +
                      "`: expected " +
                      std::to_string(typeDecl->typeParams.size()) + ", got " +
                      std::to_string(explicitTypeArgs->size()),
                  "Match the number of `[` `]` type arguments to the generic "
                  "parameter list.");
        }

        std::vector<TypeClass *> genericArgs;
        genericArgs.reserve(explicitTypeArgs->size());
        for (std::size_t i = 0; i < explicitTypeArgs->size(); ++i) {
            auto *type = requireType(
                explicitTypeArgs->at(i), explicitTypeArgs->at(i)->loc,
                "unknown generic type argument at index " +
                    std::to_string(i) + " for `" + typeName + "`");
            genericArgs.push_back(type);
        }

        if (ownerContextUnit(ownerInterface)) {
            auto *structType = instantiateGenericStructType(
                *typeDecl, genericArgs, node->loc, ownerInterface);
            auto normalizedArgs = normalizeCallArgs(node->args, node->loc);
            return lowerResolvedCall(
                makeHIR<HIRValue>(new TypeObject(structType), node->loc),
                std::move(normalizedArgs), node->loc, true);
        }

        diagnoseGenericTypeInstantiationPending(typeName, node->loc);
    }

    HIRExpr *lowerResolvedCall(HIRExpr *callee, CallArgList normalizedArgs,
                               const location &callLoc,
                               bool allowImplicitDeref) {
        auto callAttempt = resolveCallWithImplicitDeref(
            callee, std::move(normalizedArgs), callLoc, allowImplicitDeref);
        callee = callAttempt.callee;
        auto resolution = std::move(callAttempt.resolution);
        switch (resolution.kind) {
            case CallResolutionKind::ConstructorCall: {
                auto *structType =
                    resolution.callee.asType()
                        ? asUnqualified<StructType>(resolution.callee.asType())
                        : nullptr;
                if (!structType) {
                    internalError(
                        callLoc,
                        "constructor resolution is missing its struct type",
                        "This looks like a compiler pipeline bug.");
                }

                auto members = orderedStructMembers(structType, callLoc);
                std::vector<FormalCallArg> formals;
                formals.reserve(members.size());
                for (std::size_t i = 0; i < members.size(); ++i) {
                    formals.push_back({&members[i].first, members[i].second,
                                       BindingKind::Value,
                                       FormalCallArgKind::ConstructorField, i});
                }
                auto boundArgs =
                    bindCallArgs(resolution.args, formals,
                                 {callLoc, CallBindingTargetKind::Constructor,
                                  structType, true});

                std::vector<HIRExpr *> fields;
                fields.reserve(boundArgs.size());
                for (const auto &arg : boundArgs) {
                    fields.push_back(arg.expr);
                }
                return makeHIR<HIRStructLiteral>(std::move(fields), structType,
                                                 callLoc);
            }
            case CallResolutionKind::ArrayIndex: {
                auto *arrayType = asUnqualified<ArrayType>(callee->getType());
                auto *indexableType =
                    asUnqualified<IndexablePointerType>(callee->getType());
                const auto indexArity = arrayType ? arrayType->indexArity()
                                                  : (indexableType ? 1u : 0u);
                auto *elementType = resolution.resultEntity.valueType();
                if (!arrayType && !indexableType) {
                    internalError(
                        callLoc,
                        "array index resolution is missing its indexable type",
                        "This looks like a compiler pipeline bug.");
                }
                if (arrayType && !arrayType->hasStaticLayout()) {
                    error(callLoc,
                          "array indexing requires fixed explicit dimensions "
                          "or an indexable pointer",
                          "Use positive integer literal dimensions like "
                          "`i32[4]`, or an indexable pointer like `T[*]` and "
                          "write `ptr(i)`.");
                }
                std::vector<FormalCallArg> formals;
                formals.reserve(indexArity);
                for (size_t i = 0; i < indexArity; ++i) {
                    formals.push_back({nullptr, i32Ty, BindingKind::Value,
                                       FormalCallArgKind::ArrayIndex, i});
                }
                auto boundArgs =
                    bindCallArgs(resolution.args, formals,
                                 {callLoc, CallBindingTargetKind::ArrayIndex,
                                  nullptr, false});
                std::vector<HIRExpr *> args;
                args.reserve(boundArgs.size());
                for (const auto &arg : boundArgs) {
                    args.push_back(arg.expr);
                }
                return makeHIR<HIRIndex>(callee, std::move(args), elementType,
                                         callLoc);
            }
            case CallResolutionKind::FunctionCall:
            case CallResolutionKind::FunctionPointerCall: {
                auto *funcType = resolution.callType;
                if (!funcType) {
                    internalError(
                        callLoc,
                        "call resolution is missing its function type",
                        "This looks like a compiler pipeline bug.");
                }

                const auto &paramTypes = funcType->getArgTypes();
                std::vector<FormalCallArg> formals;
                formals.reserve(paramTypes.size() - resolution.argOffset);
                for (size_t i = 0; i + resolution.argOffset < paramTypes.size();
                     ++i) {
                    const string *paramName =
                        resolution.paramNames &&
                                i < resolution.paramNames->size()
                            ? &resolution.paramNames->at(i)
                            : nullptr;
                    formals.push_back(
                        {paramName, paramTypes[i + resolution.argOffset],
                         funcType->getArgBindingKind(i + resolution.argOffset),
                         FormalCallArgKind::FunctionParameter, i});
                }
                auto boundArgs = bindCallArgs(
                    resolution.args, formals,
                    {callLoc, CallBindingTargetKind::FunctionCall, nullptr,
                     resolution.paramNames && !resolution.paramNames->empty()});

                std::vector<HIRExpr *> args;
                args.reserve(boundArgs.size());
                for (const auto &arg : boundArgs) {
                    args.push_back(arg.expr);
                }

                auto *retType = funcType->getRetType();
                return makeHIR<HIRCall>(callee, std::move(args), retType,
                                        callLoc);
            }
            case CallResolutionKind::NotCallable:
                diagnoseCallFailure(callee, callLoc, resolution);
            default:
                internalError(
                    callLoc,
                    "call resolution produced an unsupported result kind",
                    "This looks like a compiler pipeline bug.");
        }
    }

    HIRExpr *analyzeTraitQualifiedCall(AstFieldCall *node,
                                       AstDotLike *calleeSyntax,
                                       const ResolvedEntityRef *traitBinding) {
        auto normalizedArgs = normalizeCallArgs(node->args, node->loc);
        if (normalizedArgs.empty()) {
            error(node->loc,
                  "trait-qualified call requires the receiver as its first "
                  "argument",
                  "Write calls like `Trait.method(&value, ...)`, or pass an "
                  "existing `Type*` receiver pointer.");
        }

        const auto *traitDecl = requireVisibleTraitDecl(
            traitBinding, calleeSyntax ? calleeSyntax->loc : node->loc,
            calleeSyntax ? calleeSyntax->parent : nullptr);
        const auto fieldName = toStdString(
            calleeSyntax ? calleeSyntax->field->text : string());
        const auto *traitMethod =
            traitDecl->findMethod(fieldName);
        if (!traitMethod) {
            error(node->loc,
                  "unknown trait method `" +
                      toStdString(traitBinding->resolvedName()) + "." +
                      fieldName + "`",
                  "Check the trait method name, or update the trait "
                  "declaration.");
        }

        const auto &receiverSpec = normalizedArgs.front();
        if (receiverSpec.name.has_value()) {
            error(receiverSpec.syntax ? receiverSpec.syntax->loc : node->loc,
                  "trait-qualified receiver must be passed positionally",
                  "Write `Trait.method(&value, ...)`, not a named receiver "
                  "argument.");
        }
        if (receiverSpec.bindingKind == BindingKind::Ref) {
            error(receiverSpec.syntax ? receiverSpec.syntax->loc : node->loc,
                  "trait-qualified receiver cannot be passed with `ref`",
                  "Reference binding rules apply to the trait method's "
                  "declared parameters, not to the explicit self pointer.");
        }

        auto *receiverPointer = requireNonCallExpr(receiverSpec.value);
        auto *receiverPointerType =
            asUnqualified<PointerType>(receiverPointer->getType());
        if (!receiverPointerType) {
            error(receiverSpec.loc,
                  "trait-qualified receiver must be passed as an explicit "
                  "self pointer",
                  "Write `Trait.method(&value, ...)` for values, or "
                  "`Trait.method(ptr, ...)` when you already have a "
                  "concrete `Type*`.");
        }
        auto *receiverStructType = asUnqualified<StructType>(
            receiverPointerType->getPointeeType());
        if (!receiverStructType) {
            error(receiverSpec.loc,
                  "trait-qualified call expects a concrete struct self pointer "
                  "for trait `" +
                      toStdString(traitBinding->resolvedName()) + "`",
                  "Pass `&value` or a concrete `Type*` that implements the "
                  "trait.");
        }
        requireTraitMethodWritableReceiver(
            *traitMethod, receiverPointerType->getPointeeType(),
            receiverSpec.loc,
            "Static trait setter calls require a writable self pointer. Borrow "
            "a writable value with `&value`, or pass a writable `Type*`.");
        auto *receiver = implicitDeref(receiverPointer, receiverSpec.loc);
        if (!receiver) {
            internalError(receiverSpec.loc,
                          "trait-qualified call failed to dereference its "
                          "explicit self pointer",
                          "This looks like a trait-qualified receiver "
                          "lowering bug.");
        }

        auto visibleImpls = unit->findVisibleTraitImpls(
            traitBinding->resolvedName(), receiverStructType);
        if (visibleImpls.empty()) {
            error(receiverSpec.loc,
                  "type `" + describeResolvedType(receiverStructType) +
                      "` does not implement trait `" +
                      toStdString(traitBinding->resolvedName()) + "`",
                  "Add `impl " + toStdString(traitBinding->resolvedName()) +
                      " for " + describeResolvedType(receiverStructType) +
                      " { ... }` in a visible module.");
        }
        ensureVisibleTraitImplBodyMethods(visibleImpls, receiverStructType,
                                          receiverSpec.loc,
                                          toStringRef(fieldName), false, true);

        auto methodLookupName = resolveConcreteTraitMethodLookupName(
            receiverStructType, *traitDecl, fieldName);
        if (methodLookupName.empty()) {
            error(receiverSpec.loc,
                  "trait `" + toStdString(traitBinding->resolvedName()) +
                      "` is missing method implementation `" + fieldName +
                      "` for `" + describeResolvedType(receiverStructType) +
                      "`",
                  "Define it in `impl " +
                      displayTraitReceiverSegment(
                          toStringRef(traitBinding->resolvedName())) +
                      " for " + describeResolvedType(receiverStructType) +
                      " { ... }`.");
        }

        CallArgList remainingArgs;
        remainingArgs.reserve(normalizedArgs.size() - 1);
        for (std::size_t i = 1; i < normalizedArgs.size(); ++i) {
            remainingArgs.push_back(normalizedArgs[i]);
        }
        return buildConcreteTraitMethodCall(
            node, *traitDecl, *traitMethod, receiver, receiverStructType,
            toStringRef(methodLookupName), remainingArgs,
            calleeSyntax ? calleeSyntax->loc : node->loc);
    }

    HIRExpr *tryAnalyzeReceiverTraitQualifiedCall(AstFieldCall *node,
                                                  AstDotLike *calleeSyntax) {
        auto *traitSelector =
            calleeSyntax && calleeSyntax->parent
                ? calleeSyntax->parent->as<AstDotLike>()
                : nullptr;
        if (!traitSelector) {
            return nullptr;
        }
        if (auto *binding = resolved.dotLike(traitSelector);
            binding && binding->kind() == ResolvedEntityRef::Kind::Trait) {
            return nullptr;
        }

        auto *traitDecl =
            findVisibleReceiverTraitDecl(toStringRef(traitSelector->field->text));
        if (!traitDecl) {
            return nullptr;
        }

        auto fieldName = toStdString(calleeSyntax->field->text);
        const auto *traitMethod =
            findTraitMethodDecl(*traitDecl, toStringRef(fieldName));
        if (!traitMethod) {
            error(calleeSyntax->loc,
                  "unknown trait method `" +
                      displayTraitReceiverSegment(
                          toStringRef(traitDecl->exportedName)) +
                      "." + fieldName + "`",
                  "Check the trait method name, or update the trait "
                  "declaration.");
        }

        auto *receiverExpr = requireExpr(traitSelector->parent);
        auto receiverAttempt = lookupMemberWithImplicitDeref(
            receiverExpr, toStdString(traitSelector->field->text),
            traitSelector->loc, !isExplicitDerefSyntax(traitSelector->parent));
        if (receiverAttempt.lookup.result.kind != LookupResultKind::NotFound) {
            return nullptr;
        }

        auto *receiver = receiverAttempt.parent;
        auto *receiverStructType =
            receiver ? asUnqualified<StructType>(receiver->getType()) : nullptr;
        if (!receiverStructType) {
            error(traitSelector->loc,
                  "trait receiver path expects a concrete struct value for `" +
                      displayTraitReceiverSegment(
                          toStringRef(traitDecl->exportedName)) +
                      "`",
                  "Write `value.Trait.method(...)` on a struct value, or "
                  "dereference a concrete `Type*` first.");
        }

        requireTraitMethodWritableReceiver(
            *traitMethod, receiver->getType(), traitSelector->loc,
            "Setter trait calls through `value.Trait.method(...)` require a "
            "writable receiver. Use a writable value, or dereference a "
            "writable pointer before the trait path.");

        auto visibleImpls =
            unit->findVisibleTraitImpls(traitDecl->exportedName,
                                        receiverStructType);
        if (visibleImpls.empty()) {
            error(traitSelector->loc,
                  "type `" + describeResolvedType(receiverStructType) +
                      "` does not implement trait `" +
                      toStdString(traitDecl->exportedName) + "`",
                  "Add `impl " + toStdString(traitDecl->exportedName) + " for " +
                      describeResolvedType(receiverStructType) +
                      " { ... }` in a visible module.");
        }
        ensureVisibleTraitImplBodyMethods(visibleImpls, receiverStructType,
                                          traitSelector->loc,
                                          toStringRef(fieldName), false, true);

        auto methodLookupName = resolveConcreteTraitMethodLookupName(
            receiverStructType, *traitDecl, fieldName);
        if (methodLookupName.empty()) {
            error(traitSelector->loc,
                  "trait `" + toStdString(traitDecl->exportedName) +
                      "` is missing method implementation `" + fieldName +
                      "` for `" + describeResolvedType(receiverStructType) +
                      "`",
                  "Define it in `impl " +
                      displayTraitReceiverSegment(
                          toStringRef(traitDecl->exportedName)) +
                      " for " + describeResolvedType(receiverStructType) +
                      " { ... }`.");
        }

        auto normalizedArgs = normalizeCallArgs(node->args, node->loc);
        return buildConcreteTraitMethodCall(
            node, *traitDecl, *traitMethod, receiver, receiverStructType,
            toStringRef(methodLookupName), normalizedArgs, calleeSyntax->loc);
    }

    HIRExpr *analyzeTraitObjectCall(AstFieldCall *node, AstDotLike *calleeSyntax,
                                    HIRExpr *receiver) {
        if (!calleeSyntax || !receiver) {
            internalError(node ? node->loc : location(),
                          "trait-object call is missing its receiver syntax",
                          "This looks like a trait-object call bug.");
        }

        auto *traitDecl = requireVisibleDynTraitDecl(
            receiver->getType(), calleeSyntax->loc, "trait object call");
        const auto methodName = toStdString(calleeSyntax->field->text);
        std::size_t slotIndex = 0;
        const auto *traitMethod =
            findTraitMethodDecl(*traitDecl, toStringRef(methodName), &slotIndex);
        if (!traitMethod) {
            error(calleeSyntax->loc,
                  "unknown trait method `" +
                      toStdString(traitDecl->exportedName) + "." +
                      methodName + "`",
                  "Check the trait method name, or update the trait "
                  "declaration.");
        }
        requireTraitMethodWritableReceiver(
            *traitMethod, receiver->getType(), calleeSyntax->loc,
            "Read-only trait objects can only call get-only methods. "
            "Construct the trait object from a writable source before calling "
            "this setter.");

        std::vector<FormalCallArg> formals;
        formals.reserve(traitMethod->paramTypeSpellings.size());
        for (std::size_t i = 0; i < traitMethod->paramTypeSpellings.size();
             ++i) {
            auto *paramType = resolveTraitMethodTypeBySpelling(
                traitMethod->paramTypeSpellings[i], calleeSyntax->loc,
                "trait object call parameter type");
            const string *paramName =
                i < traitMethod->paramNames.size()
                    ? &traitMethod->paramNames[i]
                    : nullptr;
            formals.push_back({paramName, paramType,
                               traitMethod->paramBindingKinds[i],
                               FormalCallArgKind::FunctionParameter, i});
        }

        auto normalizedArgs = normalizeCallArgs(node->args, node->loc);
        auto boundArgs = bindCallArgs(
            normalizedArgs, formals,
            {node->loc, CallBindingTargetKind::FunctionCall, nullptr,
             !traitMethod->paramNames.empty()});

        std::vector<HIRExpr *> args;
        args.reserve(boundArgs.size());
        for (const auto &arg : boundArgs) {
            args.push_back(arg.expr);
        }

        auto *slotFuncType = getOrCreateTraitDynSlotType(*traitMethod, node->loc);
        auto *retType = slotFuncType ? slotFuncType->getRetType() : nullptr;
        return makeHIR<HIRTraitObjectCall>(receiver, traitDecl->exportedName,
                                           methodName, slotIndex, slotFuncType,
                                           std::move(args), retType, node->loc);
    }

    HIRExpr *analyzeFuncRef(AstFuncRef *node) {
        auto functionName = describeGenericCallable(node->value);
        auto *binding = resolved.functionRef(node);
        if (!binding || !binding->valid()) {
            internalError(node->loc,
                          "missing resolved function reference for `" +
                              functionName + "`",
                          "Run name resolution before HIR lowering.");
        }
        auto *explicitTypeArgs = funcRefExplicitTypeArgs(node);
        if (explicitTypeArgs && !explicitTypeArgs->empty() &&
            binding->kind() != ResolvedEntityRef::Kind::GenericFunction) {
            diagnoseGenericTypeApplyTarget(node->loc);
        }
        if (binding->kind() == ResolvedEntityRef::Kind::GenericFunction) {
            if (!explicitTypeArgs || explicitTypeArgs->empty()) {
                diagnoseGenericFunctionValueUse(functionName, node->loc);
            }

            auto *functionDecl = binding->functionDecl();
            if (!functionDecl || !functionDecl->isGeneric()) {
                internalError(node->loc,
                              "generic function reference is missing template "
                              "metadata for `" +
                                  functionName + "`",
                              "Run interface collection before HIR lowering.");
            }

            if (explicitTypeArgs->size() != functionDecl->typeParams.size()) {
                error(node->loc,
                      "generic type argument count mismatch for `" +
                          functionName + "`: expected " +
                          std::to_string(functionDecl->typeParams.size()) +
                          ", got " + std::to_string(explicitTypeArgs->size()),
                      "Match the number of `[` `]` type arguments to the "
                      "generic parameter list.");
            }

            std::unordered_map<std::string, TypeClass *> genericArgs;
            genericArgs.reserve(functionDecl->typeParams.size());
            for (std::size_t i = 0; i < explicitTypeArgs->size(); ++i) {
                auto *type = requireType(
                    explicitTypeArgs->at(i), explicitTypeArgs->at(i)->loc,
                    "unknown generic type argument at index " +
                        std::to_string(i) + " for `" + functionName + "`");
                genericArgs.emplace(
                    toStdString(functionDecl->typeParams[i].localName), type);
            }
            enforceGenericTraitBounds(
                functionDecl->typeParams, genericArgs, node->loc,
                "generic function reference `" + functionName + "`");

            if (ownerContextUnit(binding->ownerInterface())) {
                auto *func = instantiateGenericFunction(
                    *functionDecl, genericArgs, node->loc,
                    binding->ownerInterface());
                auto *funcType =
                    func->getType() ? func->getType()->as<FuncType>() : nullptr;
                if (!funcType) {
                    internalError(
                        node->loc,
                        "instantiated generic function reference `" +
                            functionName + "` is missing its concrete type",
                        "This looks like a generic instantiation bug.");
                }
                auto *pointerType = typeMgr->createPointerType(funcType);
                auto *value =
                    pointerType->newObj(Object::REG_VAL | Object::READONLY);
                value->bindllvmValue(func->getllvmValue());
                return makeHIR<HIRValue>(value, node->loc);
            }

            diagnoseGenericInstantiationPending(functionName, node->loc);
        }

        auto *func = requireGlobalFunction(binding->resolvedName(), node->loc,
                                           "function reference");
        auto *funcType =
            func->getType() ? func->getType()->as<FuncType>() : nullptr;
        if (!funcType) {
            internalError(node->loc,
                          "invalid resolved function reference target `" +
                              functionName + "`",
                          "This looks like a compiler pipeline bug.");
        }

        auto *pointerType = typeMgr->createPointerType(funcType);
        auto *value = pointerType->newObj(Object::REG_VAL | Object::READONLY);
        value->bindllvmValue(func->getllvmValue());
        return makeHIR<HIRValue>(value, node->loc);
    }

    HIRExpr *analyzeAssign(AstAssign *node) {
        auto *left = requireNonCallExpr(node->left);
        if (!isAddressable(left)) {
            error(node->left ? node->left->loc : node->loc,
                  "assignment expects an addressable value on the left side",
                  "You can assign to variables, struct fields, dereferenced "
                  "pointers, or array indexing expressions.");
        }
        if (left && !isFullyWritableValueType(left->getType())) {
            errorReadOnlyAssignmentTarget(
                node->left ? node->left->loc : node->loc, left->getType());
        }
        auto *right =
            requireNonCallExpr(node->right, left ? left->getType() : nullptr);
        right = coerceNumericExpr(right, left ? left->getType() : nullptr,
                                  node->loc, false);
        right = coercePointerExpr(right, left ? left->getType() : nullptr,
                                  node->loc);
        auto *leftType = left->getType();
        auto *rightType = right->getType();
        if (!leftType || !rightType ||
            !isByteCopyCompatible(leftType, rightType)) {
            requireCompatibleTypes(node->loc, leftType, rightType,
                                   "assignment type mismatch");
        }
        return makeHIR<HIRAssign>(left, right, node->loc);
    }

    HIRExpr *analyzeBinOper(AstBinOper *node,
                            TypeClass *expectedType = nullptr) {
        TypeClass *contextualOperandType =
            expectedType && isNumericType(expectedType) ? expectedType
                                                        : nullptr;
        auto *left = requireNonCallExpr(node->left, contextualOperandType);
        auto *right = requireNonCallExpr(
            node->right, contextualOperandType
                             ? contextualOperandType
                             : (left ? left->getType() : nullptr));
        if (isNullLiteralExpr(left) || isNullLiteralExpr(right)) {
            if (node->op != Parser::token::LOGIC_EQUAL &&
                node->op != Parser::token::LOGIC_NOT_EQUAL) {
                error(node->loc, "`null` only supports pointer equality checks",
                      nullLiteralHint());
            }
            if (isNullLiteralExpr(left) && right &&
                isPointerLikeType(right->getType())) {
                left =
                    coercePointerExpr(left, right->getType(), node->left->loc);
            }
            if (isNullLiteralExpr(right) && left &&
                isPointerLikeType(left->getType())) {
                right =
                    coercePointerExpr(right, left->getType(), node->right->loc);
            }
            if (isNullLiteralExpr(left) && isNullLiteralExpr(right)) {
                error(node->loc,
                      "`null` comparison requires a concrete pointer operand",
                      "Compare a pointer value against `null`, for example `if "
                      "p == null`.");
            }
            if ((isNullLiteralExpr(left) &&
                 !isPointerLikeType(left->getType())) ||
                (isNullLiteralExpr(right) &&
                 !isPointerLikeType(right->getType())) ||
                (isNullLiteralExpr(left) &&
                 !isPointerLikeType(right->getType())) ||
                (isNullLiteralExpr(right) &&
                 !isPointerLikeType(left->getType()))) {
                error(node->loc,
                      "`null` can only be compared with pointer values",
                      nullLiteralHint());
            }
        }
        if (left->getType() != right->getType()) {
            auto *leftConst = node->left ? node->left->as<AstConst>() : nullptr;
            if (leftConst && leftConst->isDefaultFloatLiteral() &&
                isFloatType(right->getType())) {
                left = requireNonCallExpr(node->left, right->getType());
            }
        }
        if (left->getType() != right->getType()) {
            auto *rightConst =
                node->right ? node->right->as<AstConst>() : nullptr;
            if (rightConst && rightConst->isDefaultFloatLiteral() &&
                isFloatType(left->getType())) {
                right = requireNonCallExpr(node->right, left->getType());
            }
        }
        if (left->getType() != right->getType()) {
            if (auto *commonType = commonNumericType(typeMgr, left->getType(),
                                                     right->getType())) {
                left =
                    coerceNumericExpr(left, commonType, node->left->loc, false);
                right = coerceNumericExpr(right, commonType, node->right->loc,
                                          false);
            }
        }
        if (left->getType() != right->getType() &&
            (node->op == Parser::token::LOGIC_EQUAL ||
             node->op == Parser::token::LOGIC_NOT_EQUAL)) {
            auto *leftType = left->getType();
            auto *rightType = right->getType();
            if (isRawMemoryPointerType(leftType) &&
                isIndexablePointerType(rightType)) {
                right = coercePointerExpr(right, leftType, node->right->loc);
            } else if (isIndexablePointerType(leftType) &&
                       isRawMemoryPointerType(rightType)) {
                left = coercePointerExpr(left, rightType, node->left->loc);
            }
        }
        auto binding = operatorResolver.resolveBinary(
            node->op, left->getType(), right->getType(), node->loc);
        return makeHIR<HIRBinOper>(binding, left, right, binding.resultType,
                                   node->loc);
    }

    HIRExpr *analyzeUnaryOper(AstUnaryOper *node,
                              TypeClass *expectedType = nullptr) {
        if (isSupportedStaticLiteralInitializerExpr(node)) {
            return analyzeStaticLiteralInitializerExpr(typeMgr, ownerModule,
                                                       node, expectedType);
        }
        if (node->op == '-') {
            auto *constant = node->expr ? node->expr->as<AstConst>() : nullptr;
            if (constant && constant->isUnaryMinusOnlySignedMinLiteral()) {
                switch (constant->getType()) {
                    case AstConst::Type::I8:
                        return makeHIR<HIRValue>(
                            new ConstVar(
                                i8Ty, std::numeric_limits<std::int8_t>::min()),
                            node->loc);
                    case AstConst::Type::I16:
                        return makeHIR<HIRValue>(
                            new ConstVar(
                                i16Ty,
                                std::numeric_limits<std::int16_t>::min()),
                            node->loc);
                    case AstConst::Type::I32:
                        return makeHIR<HIRValue>(
                            new ConstVar(
                                i32Ty,
                                std::numeric_limits<std::int32_t>::min()),
                            node->loc);
                    case AstConst::Type::I64:
                        return makeHIR<HIRValue>(
                            new ConstVar(
                                i64Ty,
                                std::numeric_limits<std::int64_t>::min()),
                            node->loc);
                    default:
                        break;
                }
            }
        }
        TypeClass *contextualOperandType =
            expectedType && isNumericType(expectedType) ? expectedType
                                                        : nullptr;
        auto *value = requireNonCallExpr(node->expr, contextualOperandType);
        auto binding = operatorResolver.resolveUnary(
            node->op, value->getType(), isAddressable(value), node->loc);
        return makeHIR<HIRUnaryOper>(binding, value, binding.resultType,
                                     node->loc);
    }

    HIRNode *analyzeVarDef(AstVarDef *node) {
        if (auto *typeNode = node ? node->getTypeNode() : nullptr) {
            validateTypeNodeLayout(typeNode);
        }
        const bool isRefBinding = node->isRefBinding();

        TypeClass *type = nullptr;
        if (auto *typeNode = node->getTypeNode()) {
            type =
                requireType(typeNode, typeNode->loc, "unknown variable type");
            rejectBareFunctionStorage(type, node);
            rejectOpaqueStructStorage(type, node);
            rejectConstVariableStorage(type, node);
            rejectUninitializedFunctionPointerValueStorage(type, node);
        } else if (isRefBinding) {
            error(node->loc,
                  "reference binding `" + toStdString(node->getName()) +
                      "` requires an explicit type annotation",
                  "Write `ref name Type = value` so the alias target type is "
                  "explicit.");
        }

        HIRExpr *init = nullptr;
        if (node->withInitVal()) {
            init = isRefBinding ? requireNonCallExpr(node->getInitVal(), type)
                                : requireExpr(node->getInitVal(), type);
        }

        if (isRefBinding && !init) {
            error(node->loc,
                  "reference binding `" + toStdString(node->getName()) +
                      "` requires an initializer",
                  "Bind it to an addressable value like `ref a i32 = x`.");
        }

        if (type) {
            if (init) {
                if (isRefBinding) {
                    if (!isAddressable(init)) {
                        error(node->getInitVal() ? node->getInitVal()->loc
                                                 : node->loc,
                              "reference binding expects an addressable value",
                              "Bind references to variables, struct fields, "
                              "dereferenced pointers, or array indexing "
                              "expressions.");
                    }
                    if (!canBindReferenceType(type, init->getType())) {
                        error(node->loc,
                              "reference binding type mismatch for `" +
                                  toStdString(node->getName()) +
                                  "`: expected " + describeResolvedType(type) +
                                  ", got " +
                                  describeResolvedType(init->getType()),
                              "Reference bindings can add const to the alias "
                              "view, but they cannot drop existing const "
                              "qualifiers from the referenced storage.");
                    }
                } else {
                    auto *initExpectedType =
                        node->isReadOnlyBinding()
                            ? static_cast<TypeClass *>(typeMgr->createConstType(type))
                            : type;
                    init = coerceNumericExpr(init, initExpectedType, node->loc,
                                             false);
                    init = coercePointerExpr(init, initExpectedType, node->loc);
                    requireCompatibleTypes(node->loc, initExpectedType,
                                           init->getType(),
                                           "initializer type mismatch for `" +
                                               toStdString(node->getName()) +
                                               "`");
                }
            }
        } else if (init) {
            rejectMethodSelectorStorage(typeMgr, init, node);
            type = materializeValueType(typeMgr, init->getType());
            if (!type) {
                if (isNullLiteralExpr(init)) {
                    error(node->loc,
                          "cannot infer the type of `" +
                              toStdString(node->getName()) + "` from `null`",
                          "Add an explicit pointer type such as `var p i32* = "
                          "null`.");
                }
                auto *value = dynamic_cast<HIRValue *>(init);
                auto *object = value ? value->getValue() : nullptr;
                if (object && object->as<TypeObject>()) {
                    error(node->loc,
                          "type names can't be stored as runtime values",
                          "Call the type like `Vec2(...)`, or use it in a type "
                          "annotation.");
                }
                error(
                    node->loc,
                    "this expression doesn't produce a storable runtime value");
            }
            rejectBareFunctionStorage(type, node);
            rejectOpaqueStructStorage(type, node);
        } else {
            error(node->loc,
                  "cannot infer the type of `" + toStdString(node->getName()) +
                      "` without an initializer",
                  "Add an explicit type annotation or provide an initializer.");
        }

        if (node->isReadOnlyBinding()) {
            if (isRefBinding) {
                internalError(
                    node->loc,
                    "read-only variable binding unexpectedly used with `ref`",
                    "Keep `const name = expr` as a value binding only.");
            }
            type = typeMgr->createConstType(type);
        }

        auto *binding = resolved.variable(node);
        if (!binding) {
            internalError(node->loc,
                          "missing resolved variable binding for `" +
                              toStdString(node->getName()) + "`",
                          "Run name resolution before HIR lowering.");
        }
        auto *obj =
            type->newObj(Object::VARIABLE |
                         (isRefBinding ? Object::REF_ALIAS : Object::EMPTY));
        bindObject(binding, obj);
        return makeHIR<HIRVarDef>(binding->name(), obj, init, node->loc);
    }

    HIRNode *analyzeRet(AstRet *node) {
        auto *retType = hirFunc->getFuncType()->getRetType();
        HIRExpr *expr = nullptr;
        if (node->expr) {
            expr = requireNonCallExpr(node->expr, retType);
            if (!retType) {
                error(node->loc, "unexpected return value in void function");
            }
            expr = coerceNumericExpr(expr, retType, node->expr->loc, false);
            expr = coercePointerExpr(expr, retType, node->expr->loc);
            requireCompatibleTypes(node->loc, retType, expr->getType(),
                                   "return type mismatch");
        } else if (retType) {
            error(node->loc, "missing return value");
        }
        return makeHIR<HIRRet>(expr, node->loc);
    }

    HIRNode *analyzeBreak(AstBreak *node) {
        if (loopDepth <= 0) {
            error(node->loc, "`break` can only appear inside `for` loops");
        }
        return makeHIR<HIRBreak>(node->loc);
    }

    HIRNode *analyzeContinue(AstContinue *node) {
        if (loopDepth <= 0) {
            error(node->loc, "`continue` can only appear inside `for` loops");
        }
        return makeHIR<HIRContinue>(node->loc);
    }

    HIRNode *analyzeIf(AstIf *node) {
        auto *cond = requireNonCallExpr(node->condition);
        if (!isTruthyScalarType(cond->getType())) {
            error(node->condition ? node->condition->loc : node->loc,
                  "if condition expects a scalar truthy value",
                  "Use `bool`, numeric values, or pointers in condition "
                  "expressions.");
        }
        auto *thenBlock = analyzeBlock(node->then);
        auto *elseBlock = node->hasElse() ? analyzeBlock(node->els) : nullptr;
        return makeHIR<HIRIf>(cond, thenBlock, elseBlock, node->loc);
    }

    HIRNode *analyzeFor(AstFor *node) {
        auto *cond = requireNonCallExpr(node->expr);
        if (!isTruthyScalarType(cond->getType())) {
            error(
                node->expr ? node->expr->loc : node->loc,
                "for condition expects a scalar truthy value",
                "Use `bool`, numeric values, or pointers in loop conditions.");
        }
        ++loopDepth;
        auto *body = analyzeBlock(node->body);
        --loopDepth;
        auto *elseBlock = node->hasElse() ? analyzeBlock(node->els) : nullptr;
        return makeHIR<HIRFor>(cond, body, elseBlock, node->loc);
    }

    HIRExpr *analyzeDotLike(AstDotLike *node) {
        if (auto *traitBinding = resolvedTraitBinding(node->parent)) {
            error(node->loc,
                  "trait-qualified member selectors can only be used as "
                  "direct call callees",
                  "Write `" + toStdString(traitBinding->resolvedName()) +
                      "." + toStdString(node->field->text) +
                      "(&value, ...)`.");
        }
        if (auto *resolvedDotLike = analyzeResolvedDotLike(node)) {
            return resolvedDotLike;
        }

        auto *parent = requireExpr(node->parent);
        if (auto *dynTraitType = asUnqualified<DynTraitType>(parent->getType())) {
            auto *traitDecl = requireVisibleDynTraitDecl(
                dynTraitType, node->loc, "trait object member lookup");
            auto fieldName = toStdString(node->field->text);
            if (traitDecl->findMethod(fieldName)) {
                error(node->loc,
                      "trait-object method selectors can only be used as "
                      "direct call callees",
                      "Write `value." + fieldName + "(...)` on `" +
                          describeResolvedType(parent->getType()) + "`.");
            }
            error(node->loc,
                  "unknown trait method `" +
                      toStdString(traitDecl->exportedName) + "." + fieldName +
                      "`",
                  "Check the trait method name, or update the trait "
                  "declaration.");
        }
        auto fieldName = toStdString(node->field->text);
        auto attempt = lookupMemberWithImplicitDeref(
            parent, fieldName, node->loc, !isExplicitDerefSyntax(node->parent));
        if (auto *expr = materializeMemberExpr(attempt.parent, fieldName,
                                               attempt.lookup, node->loc)) {
            return expr;
        }
        diagnoseMemberLookupFailure(attempt.lookup, fieldName, node->loc,
                                    describeMemberOwnerSyntax(node->parent));
    }

    HIRExpr *analyzeCall(AstFieldCall *node,
                         TypeClass *expectedType = nullptr) {
        (void)expectedType;
        if (auto *typeApplyNode =
                node->value ? node->value->as<AstTypeApply>() : nullptr) {
            if (auto *dotLikeNode = typeApplyNode->value
                                        ? typeApplyNode->value->as<AstDotLike>()
                                        : nullptr) {
                if (auto *genericMethodCall = tryAnalyzeGenericMethodCall(
                        node, dotLikeNode, typeApplyNode->typeArgs)) {
                    return genericMethodCall;
                }
            }
            if (auto *binding =
                    resolvedGenericFunctionBinding(typeApplyNode->value)) {
                return analyzeGenericFunctionCall(
                    node, binding->functionDecl(), typeApplyNode->typeArgs,
                    describeGenericCallable(typeApplyNode->value),
                    binding->ownerInterface());
            }
            if (auto *binding = resolvedEntityBinding(typeApplyNode->value);
                binding && binding->kind() == ResolvedEntityRef::Kind::GenericType) {
                return analyzeGenericTypeCall(
                    node, binding->typeDecl(), typeApplyNode->typeArgs,
                    toStdString(binding->resolvedName()),
                    binding->ownerInterface());
            }
            diagnoseGenericTypeApplyTarget(typeApplyNode->loc);
        }
        auto normalizedArgs = normalizeCallArgs(node->args, node->loc);
        HIRExpr *callee = nullptr;
        if (auto *fieldNode =
                node->value ? node->value->as<AstField>() : nullptr) {
            auto *binding = resolved.field(fieldNode);
            if (binding && binding->kind() == ResolvedEntityRef::Kind::Module) {
                diagnoseModuleNamespaceCall(toStdString(fieldNode->name),
                                            node->loc);
            }
            if (binding && binding->kind() == ResolvedEntityRef::Kind::Trait) {
                diagnoseTraitNamespaceCall(toStdString(binding->resolvedName()),
                                           node->loc);
            }
            if (binding &&
                binding->kind() == ResolvedEntityRef::Kind::GenericFunction) {
                return analyzeGenericFunctionCall(
                    node, binding->functionDecl(), nullptr,
                    toStdString(fieldNode->name), binding->ownerInterface());
            }
            if (binding &&
                binding->kind() == ResolvedEntityRef::Kind::GenericType) {
                diagnoseGenericTypeCall(toStdString(fieldNode->name), node->loc);
            }
        }
        if (auto *funcRefNode =
                node->value ? node->value->as<AstFuncRef>() : nullptr) {
            if (auto *binding = resolved.functionRef(funcRefNode);
                binding &&
                binding->kind() == ResolvedEntityRef::Kind::GenericFunction) {
                if (auto *explicitTypeArgs = funcRefExplicitTypeArgs(funcRefNode);
                    explicitTypeArgs && !explicitTypeArgs->empty()) {
                    return analyzeGenericFunctionCall(
                        node, binding->functionDecl(), explicitTypeArgs,
                        describeGenericCallable(funcRefNode->value),
                        binding->ownerInterface());
                }
                return analyzeGenericFunctionCall(
                    node, binding->functionDecl(), nullptr,
                    describeGenericCallable(funcRefNode->value),
                    binding->ownerInterface());
            }
        }
        if (auto *dotLikeNode =
                node->value ? node->value->as<AstDotLike>() : nullptr) {
            if (auto *binding = resolved.dotLike(dotLikeNode);
                binding && binding->kind() == ResolvedEntityRef::Kind::Trait) {
                diagnoseTraitNamespaceCall(toStdString(binding->resolvedName()),
                                           node->loc);
            }
            if (auto *binding = resolved.dotLike(dotLikeNode);
                binding &&
                binding->kind() == ResolvedEntityRef::Kind::GenericFunction) {
                return analyzeGenericFunctionCall(
                    node, binding->functionDecl(), nullptr,
                    describeMemberOwnerSyntax(dotLikeNode),
                    binding->ownerInterface());
            }
            if (auto *binding = resolved.dotLike(dotLikeNode);
                binding &&
                binding->kind() == ResolvedEntityRef::Kind::GenericType) {
                diagnoseGenericTypeCall(describeMemberOwnerSyntax(dotLikeNode),
                                        node->loc);
            }
            if (auto *receiverTraitCall =
                    tryAnalyzeReceiverTraitQualifiedCall(node, dotLikeNode)) {
                return receiverTraitCall;
            }
            if (auto *genericMethodCall =
                    tryAnalyzeGenericMethodCall(node, dotLikeNode)) {
                return genericMethodCall;
            }
            if (auto *traitBinding = resolvedTraitBinding(dotLikeNode->parent)) {
                return analyzeTraitQualifiedCall(node, dotLikeNode,
                                                 traitBinding);
            }
            if (auto *resolvedDotLike = analyzeResolvedDotLike(dotLikeNode)) {
                callee = resolvedDotLike;
            } else {
                auto *receiver = requireExpr(dotLikeNode->parent);
                auto *traitObjectReceiver = receiver;
                if (!asUnqualified<DynTraitType>(traitObjectReceiver->getType()) &&
                    !isExplicitDerefSyntax(dotLikeNode->parent)) {
                    if (auto *derefReceiver =
                            implicitDeref(receiver, dotLikeNode->loc)) {
                        traitObjectReceiver = derefReceiver;
                    }
                }
                if (asUnqualified<DynTraitType>(traitObjectReceiver->getType())) {
                    return analyzeTraitObjectCall(node, dotLikeNode,
                                                  traitObjectReceiver);
                }
                auto fieldName = toStdString(dotLikeNode->field->text);
                auto attempt = lookupMemberWithImplicitDeref(
                    receiver, fieldName, node->loc,
                    !isExplicitDerefSyntax(dotLikeNode->parent));
                if (attempt.lookup.result.kind ==
                    LookupResultKind::InjectedMember) {
                    assert(attempt.lookup.injectedMember.has_value());
                    if (attempt.lookup.injectedMember->kind ==
                        InjectedMemberKind::BitCopy) {
                        if (node->args && !node->args->empty()) {
                            error(node->loc,
                                  "raw bit-copy member `" + fieldName +
                                      "` does not take arguments",
                                  "Call it as `<expr>." + fieldName + "()`.");
                        }
                        return coerceBitCopyExpr(
                            attempt.parent,
                            attempt.lookup.injectedMember->resultType,
                            node->loc);
                    }
                }
                if (auto *resolvedCallee = materializeMemberExpr(
                        attempt.parent, fieldName, attempt.lookup,
                        dotLikeNode->loc, true)) {
                    callee = resolvedCallee;
                } else {
                    diagnoseMemberLookupFailure(
                        attempt.lookup, fieldName, dotLikeNode->loc,
                        describeMemberOwnerSyntax(dotLikeNode->parent));
                }
            }
        }

        if (!callee) {
            callee = requireExpr(node->value);
        }
        return lowerResolvedCall(callee, std::move(normalizedArgs), node->loc,
                                 !isExplicitDerefSyntax(node->value));
    }

public:
    FunctionAnalyzer(TypeTable *typeMgr, GlobalScope *global,
                     HIRModule *ownerModule, const CompilationUnit *unit,
                     const ResolvedFunction &resolved,
                     AnalysisLookupCache *lookupCache)
        : typeMgr(typeMgr),
          global(global),
          unit(unit),
          resolved(resolved),
          operatorResolver(typeMgr),
          ownerModule(ownerModule),
          hirFunc(nullptr),
          localLookupCache_(unit),
          lookupCache(lookupCache ? lookupCache : &localLookupCache_) {}

private:
    void prepareFunctionShell() {
        if (resolved.isTopLevelEntry()) {
            hirFunc = makeHIR<HIRFunc>(
                getOrCreateModuleEntry(global, typeMgr, unit,
                                       resolved.isLanguageEntry()),
                getOrCreateModuleEntryType(typeMgr), resolved.loc(), true,
                resolved.isLanguageEntry(), resolved.guaranteedReturn());
        } else {
            auto *lofunc = requireDeclaredFunction(resolved.loc());
            auto *funcType =
                lofunc->getType() ? lofunc->getType()->as<FuncType>() : nullptr;
            if (!funcType) {
                internalError(resolved.loc(),
                              "resolved function type is invalid",
                              "This looks like a compiler pipeline bug.");
            }
            hirFunc = makeHIR<HIRFunc>(
                llvm::cast<llvm::Function>(lofunc->getllvmValue()), funcType,
                resolved.loc(), false, false, resolved.guaranteedReturn());
        }
    }

    void bindSelfIfNeeded() {
        if (resolved.hasSelfBinding()) {
            if (!resolved.isMethod()) {
                internalError(
                    resolved.loc(),
                    "resolved self binding is missing its method parent",
                    "This looks like a compiler pipeline bug.");
            }
            auto *methodParent =
                requireStructTypeByName(resolved.methodParentTypeName(),
                                        resolved.loc(), "method parent type");
            auto *decl = resolved.decl();
            auto *receiverPointee =
                decl && decl->receiverAccess == AccessKind::GetSet
                    ? static_cast<TypeClass *>(methodParent)
                    : static_cast<TypeClass *>(
                          typeMgr->createConstType(methodParent));
            auto *selfType = typeMgr->createPointerType(receiverPointee);
            auto *selfObj = selfType->newObj(Object::VARIABLE);
            bindObject(resolved.selfBinding(), selfObj);
            hirFunc->setSelfBinding(HIRBinding{
                resolved.selfBinding()->name(),
                resolved.selfBinding()->bindingKind(),
                selfObj,
                resolved.selfBinding()->loc(),
            });
        }
    }

    void bindParameters() {
        for (auto *paramBinding : resolved.params()) {
            auto *decl = paramBinding ? paramBinding->parameterDecl() : nullptr;
            if (!decl) {
                internalError(
                    resolved.loc(),
                    "resolved parameter binding is missing its declaration",
                    "This looks like a compiler pipeline bug.");
            }
            auto *type = requireType(
                decl->typeNode,
                decl->typeNode ? decl->typeNode->loc : paramBinding->loc(),
                "unknown function argument type for `" +
                    toStdString(paramBinding->name()) + "`");
            auto *argObj =
                type->newObj(Object::VARIABLE |
                             (paramBinding->isRefBinding() ? Object::REF_ALIAS
                                                           : Object::EMPTY));
            bindObject(paramBinding, argObj);
            hirFunc->addParam(HIRBinding{
                paramBinding->name(),
                paramBinding->bindingKind(),
                argObj,
                paramBinding->loc(),
            });
        }
    }

    void analyzeBody() {
        if (!resolved.body()) {
            hirFunc->setBody(nullptr);
            return;
        }
        hirFunc->setBody(analyzeBlock(const_cast<AstNode *>(resolved.body())));
    }

public:
    HIRFunc *analyze() {
        if (hirFunc != nullptr) {
            return hirFunc;
        }
        prepareFunctionShell();
        bindSelfIfNeeded();
        bindParameters();
        analyzeBody();
        return hirFunc;
    }
};

HIRFunc *
analyzeResolvedFunction(GlobalScope *global, HIRModule *ownerModule,
                        const CompilationUnit *unit,
                        const ResolvedFunction &resolved,
                        AnalysisLookupCache *lookupCache) {
    auto *typeMgr = requireTypeTable(global);
    return FunctionAnalyzer(typeMgr, global, ownerModule, unit, resolved,
                            lookupCache)
        .analyze();
}

}  // namespace analysis_impl

}  // namespace lona
