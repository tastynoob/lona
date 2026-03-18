#include "module_interface.hh"
#include "lona/type/type.hh"
#include <functional>
#include <utility>

namespace lona {

namespace {

std::string
toStdString(const string &value) {
    return {value.tochara(), value.size()};
}

}

ModuleInterface::ModuleInterface(std::string sourcePath, std::string moduleKey,
                                 std::string moduleName, std::uint64_t sourceHash)
    : sourcePath_(std::move(sourcePath)),
      moduleKey_(std::move(moduleKey)),
      moduleName_(std::move(moduleName)),
      sourceHash_(sourceHash) {}

ModuleInterface::~ModuleInterface() = default;

std::string
ModuleInterface::exportedNameFor(const std::string &localName) const {
    return moduleName_.empty() ? localName : moduleName_ + "." + localName;
}

void
ModuleInterface::refresh(std::string sourcePath, std::string moduleKey,
                         std::string moduleName, std::uint64_t sourceHash) {
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
ModuleInterface::declareStructType(const std::string &localName) {
    auto found = localTypes_.find(localName);
    if (found != localTypes_.end()) {
        return found->second.type ? found->second.type->as<StructType>() : nullptr;
    }

    auto exportedName = exportedNameFor(localName);
    auto type = std::make_unique<StructType>(string(exportedName.c_str()));
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[exportedName] = typePtr;
    localTypes_.emplace(localName, TypeDecl{localName, exportedName, typePtr});
    return typePtr->as<StructType>();
}

bool
ModuleInterface::declareFunction(std::string localName, FuncType *type,
                                 std::vector<std::string> paramNames) {
    return localFunctions_
        .emplace(localName,
                 FunctionDecl{localName, exportedNameFor(localName), type,
                              std::move(paramNames)})
        .second;
}

PointerType *
ModuleInterface::getOrCreatePointerType(TypeClass *pointeeType) {
    if (!pointeeType) {
        return nullptr;
    }

    auto pointerName = std::string(pointeeType->full_name.tochara(),
                                   pointeeType->full_name.size()) + "*";
    auto found = derivedTypes_.find(pointerName);
    if (found != derivedTypes_.end()) {
        return found->second->as<PointerType>();
    }

    auto type = std::make_unique<PointerType>(pointeeType);
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[pointerName] = typePtr;
    return typePtr->as<PointerType>();
}

ArrayType *
ModuleInterface::getOrCreateArrayType(TypeClass *elementType,
                                      std::vector<AstNode *> dimensions) {
    if (!elementType) {
        return nullptr;
    }

    auto arrayName = toStdString(ArrayType::buildName(elementType, dimensions));
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
    auto tupleName = toStdString(TupleType::buildName(itemTypes));
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
                                         std::vector<BindingKind> argBindingKinds) {
    std::string funcTypeName = "f";
    if (retType) {
        funcTypeName += "_";
        funcTypeName.append(retType->full_name.tochara(), retType->full_name.size());
    }
    if (!argBindingKinds.empty() && argBindingKinds.size() != argTypes.size()) {
        return nullptr;
    }
    for (auto *argType : argTypes) {
        if (!argType) {
            return nullptr;
        }
    }
    for (std::size_t i = 0; i < argTypes.size(); ++i) {
        funcTypeName += ".";
        if (!argBindingKinds.empty() && argBindingKinds[i] == BindingKind::Ref) {
            funcTypeName += "&";
        }
        auto *argType = argTypes[i];
        funcTypeName.append(argType->full_name.tochara(), argType->full_name.size());
    }

    auto found = derivedTypes_.find(funcTypeName);
    if (found != derivedTypes_.end()) {
        return found->second->as<FuncType>();
    }

    auto type = std::make_unique<FuncType>(std::vector<TypeClass *>(argTypes),
                                           retType, string(funcTypeName.c_str()),
                                           std::move(argBindingKinds));
    auto *typePtr = type.get();
    ownedTypes_.push_back(std::move(type));
    derivedTypes_[funcTypeName] = typePtr;
    return typePtr->as<FuncType>();
}

const ModuleInterface::TypeDecl *
ModuleInterface::findType(const std::string &localName) const {
    auto found = localTypes_.find(localName);
    if (found == localTypes_.end()) {
        return nullptr;
    }
    return &found->second;
}

const ModuleInterface::FunctionDecl *
ModuleInterface::findFunction(const std::string &localName) const {
    auto found = localFunctions_.find(localName);
    if (found == localFunctions_.end()) {
        return nullptr;
    }
    return &found->second;
}

std::uint64_t
hashModuleSource(const std::string &content) {
    return std::hash<std::string>{}(content);
}

}  // namespace lona
