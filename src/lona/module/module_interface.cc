#include "module_interface.hh"
#include "lona/type/type.hh"
#include <functional>
#include <utility>

namespace lona {

ModuleInterface::ModuleInterface(string sourcePath, string moduleKey,
                                 string moduleName, std::uint64_t sourceHash)
    : sourcePath_(std::move(sourcePath)),
      moduleKey_(std::move(moduleKey)),
      moduleName_(std::move(moduleName)),
      sourceHash_(sourceHash) {}

ModuleInterface::~ModuleInterface() = default;

string
ModuleInterface::exportedNameFor(const ::string &localName) const {
    if (moduleName_.empty()) {
        return localName;
    }
    return moduleName_ + "." + localName;
}

string
ModuleInterface::functionSymbolNameFor(const ::string &localName,
                                       AbiKind abiKind) const {
    if (abiKind == AbiKind::C) {
        return localName;
    }
    return exportedNameFor(localName);
}

void
ModuleInterface::refresh(string sourcePath, string moduleKey,
                         string moduleName, std::uint64_t sourceHash) {
    const bool changed = sourcePath_ != sourcePath || moduleKey_ != moduleKey ||
        moduleName_ != moduleName || sourceHash_ != sourceHash;
    sourcePath_ = std::move(sourcePath);
    moduleKey_ = std::move(moduleKey);
    moduleName_ = std::move(moduleName);
    sourceHash_ = sourceHash;
    if (changed) {
        clear();
    }
}

void
ModuleInterface::clear() {
    collected_ = false;
    ownedTypes_.clear();
    derivedTypes_.clear();
    localTypes_.clear();
    localFunctions_.clear();
}

StructType *
ModuleInterface::declareStructType(const ::string &localName,
                                   StructDeclKind declKind) {
    auto found = localTypes_.find(localName);
    if (found != localTypes_.end()) {
        auto *type = found->second.type ? found->second.type->as<StructType>() : nullptr;
        if (type) {
            type->setDeclKind(declKind);
            found->second.declKind = declKind;
        }
        return type;
    }

    auto exportedName = exportedNameFor(localName);
    auto type = std::make_unique<StructType>(exportedName, declKind);
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[exportedName] = typePtr;
    localTypes_.emplace(localName,
                        TypeDecl{localName, exportedName, declKind, typePtr});
    return typePtr->as<StructType>();
}

bool
ModuleInterface::declareFunction(string localName, FuncType *type,
                                 std::vector<string> paramNames) {
    auto abiKind = type ? type->getAbiKind() : AbiKind::Native;
    return localFunctions_
        .emplace(localName,
                 FunctionDecl{localName,
                              functionSymbolNameFor(localName, abiKind),
                              abiKind, type,
                              std::move(paramNames)})
        .second;
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
ModuleInterface::getOrCreateTupleType(const std::vector<TypeClass *> &itemTypes) {
    auto tupleName = TupleType::buildName(itemTypes);
    auto found = derivedTypes_.find(tupleName);
    if (found != derivedTypes_.end()) {
        return found->second->as<TupleType>();
    }

    auto type = std::make_unique<TupleType>(std::vector<TypeClass *>(itemTypes));
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[tupleName] = typePtr;
    return typePtr->as<TupleType>();
}

FuncType *
ModuleInterface::getOrCreateFunctionType(const std::vector<TypeClass *> &argTypes,
                                         TypeClass *retType,
                                         std::vector<BindingKind> argBindingKinds,
                                         AbiKind abiKind) {
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

ModuleInterface::TopLevelLookup
ModuleInterface::lookupTopLevelName(const ::string &localName) const {
    TopLevelLookup lookup;
    if (const auto *typeDecl = findType(localName)) {
        lookup.kind = TopLevelLookupKind::Type;
        lookup.typeDecl = typeDecl;
        return lookup;
    }
    if (const auto *functionDecl = findFunction(localName)) {
        lookup.kind = TopLevelLookupKind::Function;
        lookup.functionDecl = functionDecl;
        return lookup;
    }
    return lookup;
}

std::uint64_t
hashModuleSource(const std::string &content) {
    return std::hash<std::string>{}(content);
}

}  // namespace lona
