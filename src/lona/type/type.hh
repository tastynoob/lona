#pragma once

#include "../ast/astnode.hh"
#include "../ast/array_dim.hh"
#include "../visitor.hh"
#include "../sym/object.hh"
#include <cassert>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <llvm-18/llvm/ADT/ArrayRef.h>
#include <llvm-18/llvm/ADT/StringMap.h>
#include <llvm-18/llvm/IR/DerivedTypes.h>
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/Type.h>
#include <llvm-18/llvm/Support/Casting.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm {
class TargetMachine;
}

namespace lona {

std::string normalizeTargetTriple(const std::string &triple);
bool targetUsesHostedEntry(llvm::StringRef triple);
class TypeClass;
class ConstType;
class PointerType;
class IndexablePointerType;
class StructType;
class TupleType;
class TypeTable;
class Function;

const llvm::DataLayout &defaultTargetDataLayout();
const std::string &defaultTargetTriple();
llvm::TargetMachine &targetMachineFor(llvm::StringRef triple);
void configureModuleTargetLayout(llvm::Module &module, llvm::StringRef triple);

class TypeClass {
public:
    string const full_name;
    int typeSize = 0;  // the size of the type in bytes

    TypeClass(string full_name)
        : full_name(full_name) {}

    template<typename T>
    T *as() {
        return dynamic_cast<T *>(this);
    }

    bool equal(TypeClass *t) { return this == t; }

    virtual llvm::Type* buildLLVMType(TypeTable& types) = 0;

    virtual ObjectPtr newObj(uint32_t specifiers = Object::EMPTY) {
        return new BaseVar(this, specifiers);
    }
};

class BaseType : public TypeClass {
public:
    enum Type { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, BOOL } type;
    BaseType(Type type, string full_name)
        : TypeClass(full_name), type(type) {}

    llvm::Type* buildLLVMType(TypeTable& types) override;
};

class ConstType : public TypeClass {
    TypeClass *baseType;

public:
    static string buildName(TypeClass *baseType) {
        return baseType ? baseType->full_name + " const"
                        : string("<unknown> const");
    }

    explicit ConstType(TypeClass *baseType)
        : TypeClass(buildName(baseType)), baseType(baseType) {}

    TypeClass *getBaseType() const { return baseType; }

    llvm::Type *buildLLVMType(TypeTable &types) override;
    ObjectPtr newObj(uint32_t specifiers = Object::EMPTY) override {
        if (!baseType) {
            return TypeClass::newObj(specifiers);
        }
        auto *obj = baseType->newObj(specifiers);
        if (obj) {
            obj->setType(this);
        }
        return obj;
    }
};

class StructType : public TypeClass {
public:
    // type : index
    using ValueTy = std::pair<TypeClass *, int>;

private:
    llvm::StringMap<ValueTy> members;
    llvm::StringMap<FuncType *> methodTypes;
    llvm::StringMap<std::vector<std::string>> methodParamNames;

    bool opaque = false;
    StructDeclKind declKind = StructDeclKind::Native;
public:
    StructType(llvm::StringMap<ValueTy> &&members,
               string full_name,
               StructDeclKind declKind = StructDeclKind::Native)
        : TypeClass(full_name), members(members), opaque(false),
          declKind(declKind) {}

    // create opaque struct
    StructType(string full_name,
               StructDeclKind declKind = StructDeclKind::Native)
        : TypeClass(full_name), opaque(true), declKind(declKind) {}

    bool isOpaque() const { return opaque; }
    StructDeclKind getDeclKind() const { return declKind; }
    bool isExternDecl() const { return declKind == StructDeclKind::Extern; }
    bool isReprC() const { return declKind == StructDeclKind::ReprC; }
    bool isNativeDecl() const { return declKind == StructDeclKind::Native; }
    void setDeclKind(StructDeclKind kind) { declKind = kind; }

    void complete(const llvm::StringMap<ValueTy> &newMembers) {
        members = newMembers;
        opaque = false;
    }

    void addMethodType(llvm::StringRef name, FuncType *funcType,
                       std::vector<std::string> paramNames = {}) {
        methodTypes[name] = funcType;
        methodParamNames[name] = std::move(paramNames);
    }

    ValueTy *getMember(llvm::StringRef name) {
        auto it = members.find(name);
        if (it == members.end()) {
            return nullptr;
        }
        return &it->second;
    }

    FuncType *getMethodType(llvm::StringRef name) {
        auto it = methodTypes.find(name);
        if (it == methodTypes.end()) {
            return nullptr;
        }
        return it->second;
    }

    const std::vector<std::string> *getMethodParamNames(llvm::StringRef name) const {
        auto it = methodParamNames.find(name);
        if (it == methodParamNames.end()) {
            return nullptr;
        }
        return &it->second;
    }

    const llvm::StringMap<ValueTy> &getMembers() const { return members; }
    const llvm::StringMap<FuncType *> &getMethodTypes() const { return methodTypes; }
    std::vector<ValueTy> getMembersInOrder() const {
        std::vector<ValueTy> ordered(members.size(), {nullptr, -1});
        for (const auto &member : members) {
            auto index = static_cast<size_t>(member.second.second);
            if (index >= ordered.size()) {
                continue;
            }
            ordered[index] = member.second;
        }
        return ordered;
    }

    llvm::Type *buildLLVMType(TypeTable& types) override;

    ObjectPtr newObj(uint32_t specifiers = Object::EMPTY) override {
        return new StructVar(this, specifiers);
    }
};

class TupleType : public TypeClass {
    std::vector<TypeClass *> itemTypes;

public:
    using ValueTy = std::pair<TypeClass *, int>;

    static string buildName(const std::vector<TypeClass *> &itemTypes) {
        std::string name = "<";
        for (size_t i = 0; i < itemTypes.size(); ++i) {
            if (i != 0) {
                name += ", ";
            }
            auto *itemType = itemTypes[i];
            if (itemType) {
                name.append(itemType->full_name.tochara(), itemType->full_name.size());
            } else {
                name += "<unknown>";
            }
        }
        name += ">";
        return string(name.c_str());
    }

    static std::string buildFieldName(size_t index) {
        return "_" + std::to_string(index + 1);
    }

    explicit TupleType(std::vector<TypeClass *> itemTypes)
        : TypeClass(buildName(itemTypes)), itemTypes(std::move(itemTypes)) {}

    const std::vector<TypeClass *> &getItemTypes() const { return itemTypes; }
    bool getMember(llvm::StringRef name, ValueTy &member) const;

    llvm::Type *buildLLVMType(TypeTable &types) override;
    ObjectPtr newObj(uint32_t specifiers = Object::EMPTY) override;
};

class FuncType : public TypeClass {
    std::vector<TypeClass *> argTypes;
    std::vector<BindingKind> argBindingKinds;
    TypeClass * retType = nullptr;
    AbiKind abiKind = AbiKind::Native;
public:
    static string buildName(const std::vector<TypeClass *> &argTypes,
                            TypeClass *retType,
                            const std::vector<BindingKind> &argBindingKinds = {},
                            AbiKind abiKind = AbiKind::Native) {
        std::string funcTypeName = abiKind == AbiKind::C ? "fc" : "fn";
        if (retType) {
            funcTypeName += "_";
            funcTypeName.append(retType->full_name.tochara(), retType->full_name.size());
        }
        for (std::size_t i = 0; i < argTypes.size(); ++i) {
            funcTypeName += ".";
            if (!argBindingKinds.empty() && argBindingKinds[i] == BindingKind::Ref) {
                funcTypeName += "&";
            }
            auto *argType = argTypes[i];
            if (!argType) {
                funcTypeName += "<unknown>";
                continue;
            }
            funcTypeName.append(argType->full_name.tochara(),
                                argType->full_name.size());
        }
        return string(funcTypeName.c_str());
    }

    auto& getArgTypes() const { return argTypes; }
    const auto &getArgBindingKinds() const { return argBindingKinds; }
    BindingKind getArgBindingKind(std::size_t index) const {
        if (index >= argBindingKinds.size()) {
            return BindingKind::Value;
        }
        return argBindingKinds[index];
    }
    TypeClass *getRetType() const { return retType; }
    AbiKind getAbiKind() const { return abiKind; }
    bool isExternC() const { return abiKind == AbiKind::C; }

    FuncType(std::vector<TypeClass *> &&args,
             TypeClass *retType, string full_name,
             std::vector<BindingKind> argBindingKinds = {},
             AbiKind abiKind = AbiKind::Native)
        : TypeClass(full_name),
          argTypes(args),
          argBindingKinds(std::move(argBindingKinds)),
          retType(retType),
          abiKind(abiKind) {
        if (this->argBindingKinds.empty()) {
            this->argBindingKinds.resize(argTypes.size(), BindingKind::Value);
        }
        assert(this->argBindingKinds.size() == argTypes.size());
        // if hasSroa, the first arg is the return value
    }

    llvm::Type *buildLLVMType(TypeTable& types) override;
};

class PointerType : public TypeClass {
    TypeClass *pointeeType;

public:
    static string buildName(TypeClass *pointeeType) {
        if (auto *func = pointeeType ? pointeeType->as<FuncType>() : nullptr) {
            std::string name;
            if (func->isExternC()) {
                name += "#[extern \"C\"] ";
            }
            name += "(";
            const auto &argTypes = func->getArgTypes();
            for (size_t i = 0; i < argTypes.size(); ++i) {
                if (i != 0) {
                    name += ", ";
                }
                if (func->getArgBindingKind(i) == BindingKind::Ref) {
                    name += "ref ";
                }
                if (argTypes[i]) {
                    name.append(argTypes[i]->full_name.tochara(),
                                argTypes[i]->full_name.size());
                } else {
                    name += "<unknown>";
                }
            }
            name += ":";
            if (auto *retType = func->getRetType()) {
                name += " ";
                name.append(retType->full_name.tochara(), retType->full_name.size());
            }
            name += ")";
            return string(name.c_str());
        }
        return pointeeType ? pointeeType->full_name + "*" : string("<unknown>*");
    }

    PointerType(TypeClass *pointeeType)
        : TypeClass(buildName(pointeeType)),
          pointeeType(pointeeType) {}
    TypeClass *getPointeeType() { return pointeeType; }

    llvm::Type *buildLLVMType(TypeTable& types) override;
};

class IndexablePointerType : public TypeClass {
    TypeClass *elementType;

public:
    static string buildName(TypeClass *elementType) {
        return elementType ? elementType->full_name + "[*]"
                           : string("<unknown>[*]");
    }

    explicit IndexablePointerType(TypeClass *elementType)
        : TypeClass(buildName(elementType)),
          elementType(elementType) {}

    TypeClass *getElementType() { return elementType; }
    llvm::Type *buildLLVMType(TypeTable& types) override;
};

class ArrayType : public TypeClass {
    TypeClass *elementType;
    std::vector<AstNode *> dimensions;

public:
    static string buildName(TypeClass *elementType,
                            const std::vector<AstNode *> &dimensions) {
        string name = elementType->full_name;
        name += string(describeArrayDimensions(dimensions).c_str());
        return name;
    }

    ArrayType(TypeClass *elementType, std::vector<AstNode *> dimensions = {})
        : TypeClass(buildName(elementType, dimensions)),
          elementType(elementType),
          dimensions(std::move(dimensions)) {}

    TypeClass *getElementType() { return elementType; }
    const std::vector<AstNode *> &getDimensions() const { return dimensions; }
    std::size_t indexArity() const { return arrayIndexArity(dimensions); }
    bool usesLegacyPrefixSyntax() const {
        return isLegacyArrayDimensionPrefix(dimensions);
    }
    std::vector<std::int64_t> staticDimensions(bool *ok = nullptr) const {
        std::vector<std::int64_t> values;
        values.reserve(dimensions.size());
        bool allStatic = true;
        for (auto *dimension : dimensions) {
            if (dimension == nullptr) {
                continue;
            }
            std::int64_t value = 0;
            if (!tryExtractArrayDimension(dimension, value) || value <= 0) {
                allStatic = false;
                break;
            }
            values.push_back(value);
        }
        if (ok) {
            *ok = allStatic;
        }
        if (!allStatic) {
            values.clear();
        }
        return values;
    }
    bool hasStaticLayout() const {
        bool ok = false;
        auto values = staticDimensions(&ok);
        return ok && !values.empty();
    }
    llvm::Type *buildLLVMType(TypeTable& types) override;
};

TypeClass *stripTopLevelConst(TypeClass *type);

template<typename T>
inline T *
asUnqualified(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    return storageType ? storageType->as<T>() : nullptr;
}

bool isConstQualifiedType(TypeClass *type);
bool isConstQualificationConvertible(TypeClass *targetType, TypeClass *sourceType);
TypeClass *materializeValueType(TypeTable *typeTable, TypeClass *type);
bool isFullyWritableStructFieldType(TypeClass *type);



class TypeTable {
    struct TypeMap {
        TypeClass* type; // main type
        TypeMap* pointer; // main type's pointer type
        TypeMap* array; // main type's array type
        TypeMap() : type(nullptr), pointer(nullptr), array(nullptr) {}
        TypeMap(TypeClass* type)
            : type(type), pointer(nullptr), array(nullptr) {}
    };

    static std::size_t nextInstanceId() {
        static std::size_t nextId = 1;
        return nextId++;
    }

    llvm::Module& module;
    std::size_t instanceId_;

    llvm::StringMap<TypeMap> typeMap;
    std::unordered_map<const TypeClass *, llvm::Type *> llvmTypes_;
    struct MethodBindingKey {
        const StructType *parent = nullptr;
        std::string name;

        bool operator==(const MethodBindingKey &other) const {
            return parent == other.parent && name == other.name;
        }
    };
    struct MethodBindingKeyHash {
        std::size_t operator()(const MethodBindingKey &key) const {
            return std::hash<const StructType *>{}(key.parent) ^
                (std::hash<std::string>{}(key.name) << 1);
        }
    };
    std::unordered_map<MethodBindingKey, Function *, MethodBindingKeyHash> methodFunctions_;
    std::unordered_set<llvm::StructType *> materializingStructBodies_;

    void completeStructBodyIfNeeded(StructType *structType,
                                    llvm::StructType *llvmStruct) {
        if (!structType || !llvmStruct || structType->isOpaque() ||
            !llvmStruct->isOpaque()) {
            return;
        }

        auto [_, inserted] = materializingStructBodies_.insert(llvmStruct);
        if (!inserted) {
            return;
        }

        struct Guard {
            std::unordered_set<llvm::StructType *> &active;
            llvm::StructType *type;
            ~Guard() { active.erase(type); }
        } guard{materializingStructBodies_, llvmStruct};

        // Recursive struct pointers re-enter `getLLVMType` while the outer
        // struct body is still being materialized. Reuse the placeholder
        // `llvm::StructType` instead of trying to complete it twice.
        std::vector<llvm::Type *> memberTypes;
        auto orderedMembers = structType->getMembersInOrder();
        memberTypes.reserve(orderedMembers.size());
        for (const auto &member : orderedMembers) {
            memberTypes.push_back(getLLVMType(member.first));
        }
        llvmStruct->setBody(memberTypes);
    }

public:
    TypeTable(llvm::Module& module)
        : module(module), instanceId_(nextInstanceId()) {}

    llvm::LLVMContext& getContext() { return module.getContext(); }
    llvm::Module& getModule() { return module; }
    std::size_t instanceId() const { return instanceId_; }
    llvm::Type *getLLVMType(TypeClass *type) {
        if (!type) {
            return nullptr;
        }

        auto found = llvmTypes_.find(type);
        if (found != llvmTypes_.end()) {
            if (auto *structType = type->as<StructType>()) {
                auto *llvmStruct = llvm::cast<llvm::StructType>(found->second);
                completeStructBodyIfNeeded(structType, llvmStruct);
            }
            return found->second;
        }

        if (auto *structType = type->as<StructType>()) {
            auto *llvmStruct = llvm::cast<llvm::StructType>(
                structType->buildLLVMType(*this));
            llvmTypes_[type] = llvmStruct;
            completeStructBodyIfNeeded(structType, llvmStruct);
            return llvmStruct;
        }

        auto *llvmType = type->buildLLVMType(*this);
        llvmTypes_[type] = llvmType;
        return llvmType;
    }
    llvm::FunctionType *getLLVMFunctionType(FuncType *type) {
        return llvm::cast<llvm::FunctionType>(getLLVMType(type));
    }
    std::uint64_t getTypeAllocSize(TypeClass *type) {
        if (!type || type->as<FuncType>()) {
            return 0;
        }
        if (type->typeSize > 0) {
            return static_cast<std::uint64_t>(type->typeSize);
        }
        auto *llvmType = getLLVMType(type);
        if (auto *llvmStruct = llvm::dyn_cast<llvm::StructType>(llvmType)) {
            if (llvmStruct->isOpaque()) {
                type->typeSize = 0;
                return 0;
            }
        }
        type->typeSize = static_cast<int>(module.getDataLayout().getTypeAllocSize(llvmType));
        return static_cast<std::uint64_t>(type->typeSize);
    }
    bool shouldReturnByPointer(TypeClass *type) {
        auto *storageType = stripTopLevelConst(type);
        return storageType &&
            (storageType->as<StructType>() || storageType->as<TupleType>() ||
             storageType->as<ArrayType>());
    }

    bool addType(llvm::StringRef name, TypeClass *type) {
        if (typeMap.find(name) != typeMap.end()) {
            return false;
        }
        typeMap[name] = type;
        return true;
    }

    bool addType(const ::string &name, TypeClass *type) {
        return addType(llvm::StringRef(name.tochara(), name.size()), type);
    }

    TypeClass *getType(llvm::StringRef name) {
        auto it = typeMap.find(name);
        if (it == typeMap.end()) {
            return nullptr;
        }
        return it->second.type;
    }

    TypeClass *getType(const ::string &name) {
        return getType(llvm::StringRef(name.tochara(), name.size()));
    }

    PointerType *createPointerType(TypeClass *pointeeType) {
        auto pointerName = PointerType::buildName(pointeeType);
        if (auto *type = getType(pointerName)) {
            return type->as<PointerType>();
        }
        auto *pointerType = new PointerType(pointeeType);
        addType(pointerName, pointerType);
        return pointerType;
    }

    IndexablePointerType *createIndexablePointerType(TypeClass *elementType) {
        auto typeName = IndexablePointerType::buildName(elementType);
        if (auto *type = getType(typeName)) {
            return type->as<IndexablePointerType>();
        }
        auto *indexableType = new IndexablePointerType(elementType);
        addType(typeName, indexableType);
        return indexableType;
    }

    ConstType *createConstType(TypeClass *baseType) {
        if (!baseType) {
            return nullptr;
        }
        if (auto *qualified = baseType->as<ConstType>()) {
            return qualified;
        }
        auto typeName = ConstType::buildName(baseType);
        if (auto *type = getType(typeName)) {
            return type->as<ConstType>();
        }
        auto *constType = new ConstType(baseType);
        addType(typeName, constType);
        return constType;
    }

    ArrayType *createArrayType(TypeClass *elementType,
                               std::vector<AstNode *> dimensions = {}) {
        string arrayName = ArrayType::buildName(elementType, dimensions);
        if (auto *type = getType(arrayName)) {
            return type->as<ArrayType>();
        }

        auto *arrayType = new ArrayType(elementType, std::move(dimensions));
        addType(arrayName, arrayType);
        return arrayType;
    }

    TupleType *getOrCreateTupleType(const std::vector<TypeClass *> &itemTypes) {
        auto tupleName = TupleType::buildName(itemTypes);
        if (auto *existing = getType(tupleName)) {
            return existing->as<TupleType>();
        }

        auto *tupleType = new TupleType(std::vector<TypeClass *>(itemTypes));
        addType(tupleName, tupleType);
        return tupleType;
    }

    FuncType *getOrCreateFunctionType(const std::vector<TypeClass *> &argTypes,
                                      TypeClass *retType,
                                      std::vector<BindingKind> argBindingKinds = {},
                                      AbiKind abiKind = AbiKind::Native) {
        if (!argBindingKinds.empty() && argBindingKinds.size() != argTypes.size()) {
            return nullptr;
        }
        for (auto *argType : argTypes) {
            if (!argType) {
                return nullptr;
            }
        }
        auto funcTypeName = FuncType::buildName(argTypes, retType,
                                                argBindingKinds, abiKind);
        if (auto *existing = getType(funcTypeName)) {
            return existing->as<FuncType>();
        }
        auto *funcType = new FuncType(std::vector<TypeClass *>(argTypes), retType,
                                      funcTypeName, std::move(argBindingKinds),
                                      abiKind);
        addType(funcTypeName, funcType);
        return funcType;
    }

    TypeClass *internType(TypeClass *type) {
        if (!type) {
            return nullptr;
        }
        if (type->as<BaseType>()) {
            if (auto *existing = getType(type->full_name)) {
                return existing;
            }
            addType(type->full_name, type);
            return type;
        }
        if (auto *structType = type->as<StructType>()) {
            if (auto *existing = getType(type->full_name)) {
                auto *existingStruct = existing->as<StructType>();
                if (existingStruct) {
                    existingStruct->setDeclKind(structType->getDeclKind());
                    if (existingStruct->isOpaque() && !structType->isOpaque()) {
                        existingStruct->complete(structType->getMembers());
                    }
                    for (const auto &method : structType->getMethodTypes()) {
                        if (!existingStruct->getMethodType(method.first())) {
                            std::vector<std::string> paramNames;
                            if (const auto *storedParamNames =
                                    structType->getMethodParamNames(method.first())) {
                                paramNames = *storedParamNames;
                            }
                            existingStruct->addMethodType(method.first(), method.second,
                                                          std::move(paramNames));
                        }
                    }
                }
                return existing;
            }
            addType(type->full_name, type);
            for (const auto &member : structType->getMembers()) {
                internType(member.second.first);
            }
            for (const auto &method : structType->getMethodTypes()) {
                internType(method.second);
            }
            return type;
        }
        if (auto *qualified = type->as<ConstType>()) {
            auto *baseType = internType(qualified->getBaseType());
            if (!baseType) {
                return nullptr;
            }
            if (auto *existing = getType(type->full_name)) {
                return existing;
            }
            if (baseType == qualified->getBaseType()) {
                addType(type->full_name, type);
                return type;
            }
            return createConstType(baseType);
        }
        if (auto *pointer = type->as<PointerType>()) {
            auto *pointeeType = internType(pointer->getPointeeType());
            if (!pointeeType) {
                return nullptr;
            }
            if (auto *existing = getType(type->full_name)) {
                return existing;
            }
            if (pointeeType == pointer->getPointeeType()) {
                addType(type->full_name, type);
                return type;
            }
            return createPointerType(pointeeType);
        }
        if (auto *indexable = type->as<IndexablePointerType>()) {
            auto *elementType = internType(indexable->getElementType());
            if (!elementType) {
                return nullptr;
            }
            if (auto *existing = getType(type->full_name)) {
                return existing;
            }
            if (elementType == indexable->getElementType()) {
                addType(type->full_name, type);
                return type;
            }
            return createIndexablePointerType(elementType);
        }
        if (auto *array = type->as<ArrayType>()) {
            auto *elementType = internType(array->getElementType());
            if (!elementType) {
                return nullptr;
            }
            if (auto *existing = getType(type->full_name)) {
                return existing;
            }
            if (elementType == array->getElementType()) {
                addType(type->full_name, type);
                return type;
            }
            return createArrayType(elementType, array->getDimensions());
        }
        if (auto *tuple = type->as<TupleType>()) {
            std::vector<TypeClass *> itemTypes;
            itemTypes.reserve(tuple->getItemTypes().size());
            bool reusedOriginalItems = true;
            for (auto *item : tuple->getItemTypes()) {
                auto *internedItem = internType(item);
                if (!internedItem) {
                    return nullptr;
                }
                reusedOriginalItems = reusedOriginalItems && internedItem == item;
                itemTypes.push_back(internedItem);
            }
            if (auto *existing = getType(type->full_name)) {
                return existing;
            }
            if (reusedOriginalItems) {
                addType(type->full_name, type);
                return type;
            }
            return getOrCreateTupleType(itemTypes);
        }
        if (auto *func = type->as<FuncType>()) {
            std::vector<TypeClass *> argTypes;
            argTypes.reserve(func->getArgTypes().size());
            bool reusedOriginalArgs = true;
            for (auto *arg : func->getArgTypes()) {
                auto *internedArg = internType(arg);
                if (!internedArg) {
                    return nullptr;
                }
                reusedOriginalArgs = reusedOriginalArgs && internedArg == arg;
                argTypes.push_back(internedArg);
            }
            auto *retType = internType(func->getRetType());
            if (auto *existing = getType(type->full_name)) {
                return existing;
            }
            if (reusedOriginalArgs && retType == func->getRetType()) {
                addType(type->full_name, type);
                return type;
            }
            return getOrCreateFunctionType(argTypes, retType,
                                           func->getArgBindingKinds(),
                                           func->getAbiKind());
        }
        return type;
    }

    void bindMethodFunction(StructType *parent, llvm::StringRef name, Function *func) {
        methodFunctions_[{parent, name.str()}] = func;
    }
    Function *getMethodFunction(const StructType *parent, llvm::StringRef name) const {
        auto found = methodFunctions_.find({parent, name.str()});
        if (found == methodFunctions_.end()) {
            return nullptr;
        }
        return found->second;
    }

    TypeClass *getType(TypeNode *node) {
        if (!node) {
            return nullptr;
        }

        if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
            return getType(param->type);
        }

        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            return getType(base->name);
        }

        if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
            auto *type = getType(pointer->base);
            for (uint32_t i = 0; type && i < pointer->dim; i++) {
                type = createPointerType(type);
            }
            return type;
        }

        if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
            auto *elementType = getType(indexable->base);
            return elementType ? createIndexablePointerType(elementType) : nullptr;
        }

        if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
            auto *elementType = getType(array->base);
            if (!elementType) {
                return nullptr;
            }
            return createArrayType(elementType, array->dim);
        }

        if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
            auto *baseType = getType(qualified->base);
            return baseType ? createConstType(baseType) : nullptr;
        }

        if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
            std::vector<TypeClass *> itemTypes;
            itemTypes.reserve(tuple->items.size());
            for (auto *item : tuple->items) {
                auto *itemType = getType(item);
                if (!itemType) {
                    return nullptr;
                }
                itemTypes.push_back(itemType);
            }
            return getOrCreateTupleType(itemTypes);
        }

        if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
            std::vector<TypeClass *> argTypes;
            std::vector<BindingKind> argBindingKinds;
            auto *retType = getType(func->ret);
            argBindingKinds.reserve(func->args.size());
            for (auto *arg : func->args) {
                argBindingKinds.push_back(funcParamBindingKind(arg));
                auto *argType = getType(unwrapFuncParamType(arg));
                if (!argType) {
                    return nullptr;
                }
                argTypes.push_back(argType);
            }
            auto *funcType = getOrCreateFunctionType(argTypes, retType,
                                                     std::move(argBindingKinds),
                                                     AbiKind::Native);
            return funcType ? createPointerType(funcType) : nullptr;
        }

        return nullptr;
    }
};

inline bool
isByteCopyPlainType(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    return storageType &&
        (storageType->as<BaseType>() || storageType->as<PointerType>() ||
         storageType->as<IndexablePointerType>());
}

inline bool
isPointerLikeType(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    return storageType &&
        (storageType->as<PointerType>() || storageType->as<IndexablePointerType>());
}

inline TypeClass *
getRawPointerPointeeType(TypeClass *type) {
    auto *pointer = asUnqualified<PointerType>(type);
    return pointer ? pointer->getPointeeType() : nullptr;
}

inline TypeClass *
getIndexablePointerElementType(TypeClass *type) {
    auto *pointer = asUnqualified<IndexablePointerType>(type);
    return pointer ? pointer->getElementType() : nullptr;
}

inline bool
isBoolStorageType(TypeClass *type) {
    auto *base = asUnqualified<BaseType>(type);
    return base && base->type == BaseType::BOOL;
}

inline bool
isNumericStorageType(TypeClass *type) {
    auto *base = asUnqualified<BaseType>(type);
    if (!base) {
        return false;
    }
    switch (base->type) {
    case BaseType::U8:
    case BaseType::I8:
    case BaseType::U16:
    case BaseType::I16:
    case BaseType::U32:
    case BaseType::I32:
    case BaseType::U64:
    case BaseType::I64:
    case BaseType::F32:
    case BaseType::F64:
        return true;
    default:
        return false;
    }
}

inline bool
isByteCopyCompatible(TypeClass *dstType, TypeClass *srcType) {
    return dstType && srcType &&
        isConstQualificationConvertible(dstType, materializeValueType(nullptr, srcType));
}

}  // namespace lona
