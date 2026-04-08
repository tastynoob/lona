#include "module_interface.hh"
#include "lona/type/type.hh"
#include <functional>
#include <utility>

namespace lona {

namespace {

std::string
normalizeExportNamespace(const string &modulePath, const string &moduleName) {
    auto path = toStdString(modulePath);
    if (path.empty()) {
        path = toStdString(moduleName);
    }
    if (path.empty()) {
        return {};
    }

    std::string normalized;
    normalized.reserve(path.size());
    bool lastWasSeparator = true;
    for (char ch : path) {
        if (ch == '/' || ch == '\\' || ch == '.') {
            if (!lastWasSeparator && !normalized.empty()) {
                normalized.push_back('.');
            }
            lastWasSeparator = true;
            continue;
        }
        normalized.push_back(ch);
        lastWasSeparator = false;
    }
    while (!normalized.empty() && normalized.back() == '.') {
        normalized.pop_back();
    }
    return normalized;
}

}  // namespace

ModuleInterface::ModuleInterface(string sourcePath, string moduleKey,
                                 string moduleName, string modulePath,
                                 std::uint64_t sourceHash)
    : sourcePath_(std::move(sourcePath)),
      moduleKey_(std::move(moduleKey)),
      moduleName_(std::move(moduleName)),
      modulePath_(std::move(modulePath)),
      sourceHash_(sourceHash) {}

ModuleInterface::~ModuleInterface() = default;

string
ModuleInterface::exportedNameFor(const ::string &localName) const {
    auto prefix = exportNamespacePrefix();
    if (prefix.empty()) {
        return localName;
    }
    return prefix + "." + localName;
}

string
ModuleInterface::functionSymbolNameFor(const ::string &localName,
                                       AbiKind abiKind) const {
    if (abiKind == AbiKind::C) {
        return localName;
    }
    return exportedNameFor(localName);
}

string
ModuleInterface::globalSymbolNameFor(const ::string &localName,
                                     bool isExtern) const {
    return isExtern ? localName : exportedNameFor(localName);
}

void
ModuleInterface::refresh(string sourcePath, string moduleKey, string moduleName,
                         string modulePath, std::uint64_t sourceHash) {
    const bool changed = sourcePath_ != sourcePath || moduleKey_ != moduleKey ||
                         moduleName_ != moduleName ||
                         modulePath_ != modulePath || sourceHash_ != sourceHash;
    sourcePath_ = std::move(sourcePath);
    moduleKey_ = std::move(moduleKey);
    moduleName_ = std::move(moduleName);
    modulePath_ = std::move(modulePath);
    sourceHash_ = sourceHash;
    if (changed) {
        clear();
    }
}

string
ModuleInterface::exportNamespacePrefix() const {
    return string(normalizeExportNamespace(modulePath_, moduleName_).c_str());
}

void
ModuleInterface::clear() {
    collected_ = false;
    ownedTypes_.clear();
    derivedTypes_.clear();
    localTypes_.clear();
    localTraits_.clear();
    traitImpls_.clear();
    localFunctions_.clear();
    localGlobals_.clear();
    importedModules_.clear();
}

StructType *
ModuleInterface::declareStructType(const ::string &localName,
                                   StructDeclKind declKind,
                                   std::vector<GenericParamDecl> typeParams) {
    auto found = localTypes_.find(localName);
    if (found != localTypes_.end()) {
        auto *type =
            found->second.type ? found->second.type->as<StructType>() : nullptr;
        if (type) {
            type->setDeclKind(declKind);
            found->second.declKind = declKind;
        }
        found->second.typeParams = std::move(typeParams);
        return type;
    }

    auto exportedName = exportedNameFor(localName);
    auto type = std::make_unique<StructType>(exportedName, declKind);
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[exportedName] = typePtr;
    localTypes_.emplace(localName, TypeDecl{localName, exportedName, declKind,
                                            typePtr, std::move(typeParams)});
    return typePtr->as<StructType>();
}

bool
ModuleInterface::declareTrait(string localName,
                              std::vector<TraitMethodDecl> methods) {
    return localTraits_
        .emplace(localName,
                 TraitDecl{localName, exportedNameFor(localName),
                           std::move(methods)})
        .second;
}

bool
ModuleInterface::defineTraitMethods(string localName,
                                    std::vector<TraitMethodDecl> methods) {
    auto found = localTraits_.find(localName);
    if (found == localTraits_.end()) {
        return false;
    }
    found->second.methods = std::move(methods);
    return true;
}

bool
ModuleInterface::declareTraitImpl(string selfTypeSpelling, TypeNode *selfTypeNode,
                                  string traitName, bool hasBody,
                                  std::vector<GenericParamDecl> typeParams,
                                  AstTraitImplDecl *syntaxDecl,
                                  std::vector<MethodTemplateDecl> bodyMethods) {
    traitImpls_.push_back(TraitImplDecl{std::move(selfTypeSpelling),
                                        selfTypeNode, std::move(traitName),
                                        hasBody, std::move(typeParams),
                                        syntaxDecl, std::move(bodyMethods)});
    return true;
}

bool
ModuleInterface::declareFunction(string localName, FuncType *type,
                                 std::vector<string> paramNames,
                                 std::vector<BindingKind> paramBindingKinds,
                                 std::vector<TypeNode *> paramTypeNodes,
                                 std::vector<string> paramTypeSpellings,
                                 TypeNode *returnTypeNode,
                                 string returnTypeSpelling,
                                 std::vector<GenericParamDecl> typeParams) {
    auto abiKind = type ? type->getAbiKind() : AbiKind::Native;
    return localFunctions_
        .emplace(
            localName,
            FunctionDecl{localName, functionSymbolNameFor(localName, abiKind),
                         abiKind, type, std::move(paramNames),
                         std::move(paramBindingKinds),
                         std::move(paramTypeNodes),
                         std::move(paramTypeSpellings), returnTypeNode,
                         std::move(returnTypeSpelling),
                         std::move(typeParams)})
        .second;
}

bool
ModuleInterface::declareStructMethodTemplate(string structLocalName,
                                             MethodTemplateDecl method) {
    auto found = localTypes_.find(structLocalName);
    if (found == localTypes_.end()) {
        return false;
    }
    found->second.methodTemplates.push_back(std::move(method));
    return true;
}

bool
ModuleInterface::declareImportedModule(string localName, string moduleKey,
                                       string moduleName,
                                       const ModuleInterface *interface) {
    ImportedModuleDecl imported{localName, std::move(moduleKey),
                                std::move(moduleName), interface};
    auto found = importedModules_.find(localName);
    if (found != importedModules_.end()) {
        found->second = std::move(imported);
        return false;
    }
    importedModules_.emplace(std::move(localName), std::move(imported));
    return true;
}

bool
ModuleInterface::declareGlobal(string localName, TypeClass *type,
                               bool isExtern) {
    return localGlobals_
        .emplace(localName,
                 GlobalDecl{localName, globalSymbolNameFor(localName, isExtern),
                            isExtern, type})
        .second;
}

AnyType *
ModuleInterface::getOrCreateAnyType() {
    constexpr auto kAnyTypeName = "any";
    auto found = derivedTypes_.find(kAnyTypeName);
    if (found != derivedTypes_.end()) {
        return found->second->as<AnyType>();
    }

    auto type = std::make_unique<AnyType>();
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[kAnyTypeName] = typePtr;
    return typePtr->as<AnyType>();
}

StructType *
ModuleInterface::getOrCreateAppliedStructType(const ::string &appliedName,
                                              StructDeclKind declKind,
                                              string appliedTemplateName,
                                              std::vector<TypeClass *> appliedTypeArgs) {
    auto found = derivedTypes_.find(appliedName);
    if (found != derivedTypes_.end()) {
        auto *structType = found->second->as<StructType>();
        if (structType) {
            structType->setDeclKind(declKind);
            if (!appliedTemplateName.empty()) {
                structType->setAppliedTemplateInfo(
                    std::move(appliedTemplateName),
                    std::move(appliedTypeArgs));
            }
        }
        return structType;
    }

    auto type = std::make_unique<StructType>(
        appliedName, declKind, std::move(appliedTemplateName),
        std::move(appliedTypeArgs));
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[appliedName] = typePtr;
    return typePtr->as<StructType>();
}

PointerType *
ModuleInterface::getOrCreatePointerType(TypeClass *pointeeType) {
    if (!pointeeType) {
        return nullptr;
    }

    auto pointerTypeName = PointerType::buildName(pointeeType);
    auto found = derivedTypes_.find(pointerTypeName);
    if (found != derivedTypes_.end()) {
        return found->second->as<PointerType>();
    }

    auto type = std::make_unique<PointerType>(pointeeType);
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[pointerTypeName] = typePtr;
    return typePtr->as<PointerType>();
}

IndexablePointerType *
ModuleInterface::getOrCreateIndexablePointerType(TypeClass *elementType) {
    if (!elementType) {
        return nullptr;
    }

    auto typeName = IndexablePointerType::buildName(elementType);
    auto found = derivedTypes_.find(typeName);
    if (found != derivedTypes_.end()) {
        return found->second->as<IndexablePointerType>();
    }

    auto type = std::make_unique<IndexablePointerType>(elementType);
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[typeName] = typePtr;
    return typePtr->as<IndexablePointerType>();
}

DynTraitType *
ModuleInterface::getOrCreateDynTraitType(const ::string &traitName,
                                         bool readOnlyDataPtr) {
    auto typeName = DynTraitType::buildName(traitName, readOnlyDataPtr);
    auto found = derivedTypes_.find(typeName);
    if (found != derivedTypes_.end()) {
        return found->second->as<DynTraitType>();
    }

    auto type = std::make_unique<DynTraitType>(traitName, readOnlyDataPtr);
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[typeName] = typePtr;
    return typePtr->as<DynTraitType>();
}

ConstType *
ModuleInterface::getOrCreateConstType(TypeClass *baseType) {
    if (!baseType) {
        return nullptr;
    }
    if (auto *qualified = baseType->as<ConstType>()) {
        return qualified;
    }

    auto typeName = ConstType::buildName(baseType);
    auto found = derivedTypes_.find(typeName);
    if (found != derivedTypes_.end()) {
        return found->second->as<ConstType>();
    }

    auto type = std::make_unique<ConstType>(baseType);
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[typeName] = typePtr;
    return typePtr->as<ConstType>();
}

ArrayType *
ModuleInterface::getOrCreateArrayType(TypeClass *elementType,
                                      std::vector<AstNode *> dimensions) {
    if (!elementType) {
        return nullptr;
    }

    auto arrayName = ArrayType::buildName(elementType, dimensions);
    auto found = derivedTypes_.find(arrayName);
    if (found != derivedTypes_.end()) {
        return found->second->as<ArrayType>();
    }

    auto type = std::make_unique<ArrayType>(elementType, std::move(dimensions));
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[arrayName] = typePtr;
    return typePtr->as<ArrayType>();
}

TupleType *
ModuleInterface::getOrCreateTupleType(
    const std::vector<TypeClass *> &itemTypes) {
    auto tupleName = TupleType::buildName(itemTypes);
    auto found = derivedTypes_.find(tupleName);
    if (found != derivedTypes_.end()) {
        return found->second->as<TupleType>();
    }

    auto type =
        std::make_unique<TupleType>(std::vector<TypeClass *>(itemTypes));
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[tupleName] = typePtr;
    return typePtr->as<TupleType>();
}

const ModuleInterface::ImportedModuleDecl *
ModuleInterface::findImportedModule(const ::string &localName) const {
    auto found = importedModules_.find(localName);
    if (found == importedModules_.end()) {
        return nullptr;
    }
    return &found->second;
}

FuncType *
ModuleInterface::getOrCreateFunctionType(
    const std::vector<TypeClass *> &argTypes, TypeClass *retType,
    std::vector<BindingKind> argBindingKinds, AbiKind abiKind) {
    if (!argBindingKinds.empty() && argBindingKinds.size() != argTypes.size()) {
        return nullptr;
    }
    for (auto *argType : argTypes) {
        if (!argType) {
            return nullptr;
        }
    }
    auto funcTypeName =
        FuncType::buildName(argTypes, retType, argBindingKinds, abiKind);

    auto found = derivedTypes_.find(funcTypeName);
    if (found != derivedTypes_.end()) {
        return found->second->as<FuncType>();
    }

    auto type = std::make_unique<FuncType>(std::vector<TypeClass *>(argTypes),
                                           retType, funcTypeName,
                                           std::move(argBindingKinds), abiKind);
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[funcTypeName] = typePtr;
    return typePtr->as<FuncType>();
}

const ModuleInterface::TypeDecl *
ModuleInterface::findType(const ::string &localName) const {
    auto found = localTypes_.find(localName);
    if (found == localTypes_.end()) {
        return nullptr;
    }
    return &found->second;
}

const ModuleInterface::FunctionDecl *
ModuleInterface::findFunction(const ::string &localName) const {
    auto found = localFunctions_.find(localName);
    if (found == localFunctions_.end()) {
        return nullptr;
    }
    return &found->second;
}

const ModuleInterface::TraitDecl *
ModuleInterface::findTrait(const ::string &localName) const {
    auto found = localTraits_.find(localName);
    if (found == localTraits_.end()) {
        return nullptr;
    }
    return &found->second;
}

const ModuleInterface::TraitDecl *
ModuleInterface::findTraitByExportedName(const ::string &exportedName) const {
    for (const auto &entry : localTraits_) {
        if (entry.second.exportedName == exportedName) {
            return &entry.second;
        }
    }
    return nullptr;
}

const ModuleInterface::TraitImplDecl *
ModuleInterface::findTraitImpl(const ::string &traitName,
                               const ::string &selfTypeSpelling) const {
    for (const auto &implDecl : traitImpls_) {
        if (implDecl.traitName == traitName &&
            implDecl.selfTypeSpelling == selfTypeSpelling) {
            return &implDecl;
        }
    }
    return nullptr;
}

const ModuleInterface::GlobalDecl *
ModuleInterface::findGlobal(const ::string &localName) const {
    auto found = localGlobals_.find(localName);
    if (found == localGlobals_.end()) {
        return nullptr;
    }
    return &found->second;
}

TypeClass *
ModuleInterface::findDerivedType(const ::string &spelling) const {
    auto found = derivedTypes_.find(spelling);
    return found == derivedTypes_.end() ? nullptr : found->second;
}

ModuleInterface::TopLevelLookup
ModuleInterface::lookupTopLevelName(const ::string &localName) const {
    TopLevelLookup lookup;
    if (const auto *typeDecl = findType(localName)) {
        lookup.kind = TopLevelLookupKind::Type;
        lookup.typeDecl = typeDecl;
        return lookup;
    }
    if (const auto *traitDecl = findTrait(localName)) {
        lookup.kind = TopLevelLookupKind::Trait;
        lookup.traitDecl = traitDecl;
        return lookup;
    }
    if (const auto *functionDecl = findFunction(localName)) {
        lookup.kind = TopLevelLookupKind::Function;
        lookup.functionDecl = functionDecl;
        return lookup;
    }
    if (const auto *globalDecl = findGlobal(localName)) {
        lookup.kind = TopLevelLookupKind::Global;
        lookup.globalDecl = globalDecl;
        return lookup;
    }
    return lookup;
}

std::uint64_t
hashModuleSource(const std::string &content) {
    return std::hash<std::string>{}(content);
}

}  // namespace lona
