#pragma once

#include "../ast/array_dim.hh"
#include "../ast/astnode.hh"
#include "../ast/type_node_string.hh"
#include "../ast/type_node_tools.hh"
#include "../sym/object.hh"
#include "../visitor.hh"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <llvm-18/llvm/ADT/ArrayRef.h>
#include <llvm-18/llvm/ADT/StringMap.h>
#include <llvm-18/llvm/ADT/StringSet.h>
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

class CompilationUnit;

inline std::string
traitMethodSlotKey(llvm::StringRef traitName, llvm::StringRef methodName) {
    return traitName.str() + "::" + methodName.str();
}

inline std::string
traitMethodSlotKey(const ::string &traitName, const ::string &methodName) {
    return traitMethodSlotKey(
        llvm::StringRef(traitName.tochara(), traitName.size()),
        llvm::StringRef(methodName.tochara(), methodName.size()));
}

std::string
normalizeTargetTriple(const std::string &triple);
bool
targetUsesHostedEntry(llvm::StringRef triple);
class TypeClass;
class AnyType;
class ConstType;
class DynTraitType;
class PointerType;
class IndexablePointerType;
class StructType;
class TupleType;
class TypeTable;
class Function;

const llvm::DataLayout &
defaultTargetDataLayout();
const std::string &
defaultTargetTriple();
llvm::TargetMachine &
targetMachineFor(llvm::StringRef triple);
void
configureModuleTargetLayout(llvm::Module &module, llvm::StringRef triple);

class TypeClass {
public:
    string const full_name;
    int typeSize = 0;  // the size of the type in bytes
    std::uint32_t refCount_ = 0;

    TypeClass(string full_name) : full_name(full_name) {}
    virtual ~TypeClass() = default;

    void retain() { ++refCount_; }
    void release() {
        assert(refCount_ > 0);
        if (--refCount_ == 0) {
            delete this;
        }
    }
    std::uint32_t refCount() const { return refCount_; }

    template<typename T>
    T *as() {
        return dynamic_cast<T *>(this);
    }

    bool equal(TypeClass *t) { return this == t; }

    virtual llvm::Type *buildLLVMType(TypeTable &types) = 0;

    virtual ObjectPtr newObj(uint32_t specifiers = Object::EMPTY) {
        return new BaseVar(this, specifiers);
    }
};

class BaseType : public TypeClass {
public:
    enum Type {
        U8,
        I8,
        U16,
        I16,
        U32,
        I32,
        U64,
        I64,
        USIZE,
        F32,
        F64,
        BOOL
    } type;
    BaseType(Type type, string full_name) : TypeClass(full_name), type(type) {}

    llvm::Type *buildLLVMType(TypeTable &types) override;
};

class AnyType : public TypeClass {
public:
    AnyType() : TypeClass("any") {}

    llvm::Type *buildLLVMType(TypeTable &types) override;
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
        auto obj = baseType->newObj(specifiers);
        if (obj) {
            obj->setType(this);
        }
        return obj;
    }
};

class DynTraitType : public TypeClass {
    string traitName_;
    bool readOnlyDataPtr_ = false;

public:
    static string buildName(const ::string &traitName,
                            bool readOnlyDataPtr = false) {
        return readOnlyDataPtr ? traitName + " const dyn"
                               : traitName + " dyn";
    }

    explicit DynTraitType(string traitName, bool readOnlyDataPtr = false)
        : TypeClass(buildName(traitName, readOnlyDataPtr)),
          traitName_(std::move(traitName)),
          readOnlyDataPtr_(readOnlyDataPtr) {}

    const string &traitName() const { return traitName_; }
    bool hasReadOnlyDataPtr() const { return readOnlyDataPtr_; }
    llvm::Type *buildLLVMType(TypeTable &types) override;
};

class StructType : public TypeClass {
public:
    // type : index
    using ValueTy = std::pair<TypeClass *, int>;
    struct TraitMethodEntry {
        string traitName;
        string methodName;
        FuncType *funcType = nullptr;
        std::vector<string> paramNames;
    };

private:
    llvm::StringMap<ValueTy> members;
    llvm::StringMap<AccessKind> memberAccess;
    llvm::StringSet<> embeddedMembers;
    llvm::StringMap<FuncType *> methodTypes;
    llvm::StringMap<std::vector<string>> methodParamNames;
    std::unordered_map<std::string, TraitMethodEntry> traitMethodTypes;

    bool opaque = false;
    StructDeclKind declKind = StructDeclKind::Native;
    string appliedTemplateName;
    std::vector<TypeClass *> appliedTypeArgs;
    const CompilationUnit *appliedTemplateOwnerUnit = nullptr;

public:
    StructType(llvm::StringMap<ValueTy> &&members, string full_name,
               StructDeclKind declKind = StructDeclKind::Native)
        : TypeClass(full_name),
          members(members),
          opaque(false),
          declKind(declKind) {}

    // create opaque struct
    StructType(string full_name,
               StructDeclKind declKind = StructDeclKind::Native,
               string appliedTemplateName = {},
               std::vector<TypeClass *> appliedTypeArgs = {})
        : TypeClass(full_name),
          opaque(true),
          declKind(declKind),
          appliedTemplateName(std::move(appliedTemplateName)),
          appliedTypeArgs(std::move(appliedTypeArgs)) {}

    bool isOpaque() const { return opaque; }
    StructDeclKind getDeclKind() const { return declKind; }
    bool isOpaqueDecl() const { return declKind == StructDeclKind::Opaque; }
    bool isReprC() const { return declKind == StructDeclKind::ReprC; }
    bool isNativeDecl() const { return declKind == StructDeclKind::Native; }
    void setDeclKind(StructDeclKind kind) { declKind = kind; }
    bool isAppliedTemplateInstance() const { return !appliedTemplateName.empty(); }
    const string &getAppliedTemplateName() const { return appliedTemplateName; }
    const std::vector<TypeClass *> &getAppliedTypeArgs() const {
        return appliedTypeArgs;
    }
    const CompilationUnit *getAppliedTemplateOwnerUnit() const {
        return appliedTemplateOwnerUnit;
    }
    void setAppliedTemplateInfo(string templateName,
                                std::vector<TypeClass *> typeArgs,
                                const CompilationUnit *templateOwnerUnit = nullptr) {
        appliedTemplateName = std::move(templateName);
        appliedTypeArgs = std::move(typeArgs);
        appliedTemplateOwnerUnit = templateOwnerUnit;
    }

    void complete(const llvm::StringMap<ValueTy> &newMembers,
                  const llvm::StringMap<AccessKind> &newMemberAccess = {},
                  const llvm::StringSet<> &newEmbeddedMembers = {}) {
        members = newMembers;
        memberAccess = newMemberAccess;
        embeddedMembers = newEmbeddedMembers;
        opaque = false;
    }

    void addMethodType(llvm::StringRef name, FuncType *funcType,
                       std::vector<string> paramNames = {}) {
        methodTypes[name] = funcType;
        methodParamNames[name] = std::move(paramNames);
    }

    void addTraitMethodType(llvm::StringRef traitName, llvm::StringRef methodName,
                            FuncType *funcType,
                            std::vector<string> paramNames = {}) {
        auto key = traitMethodSlotKey(traitName, methodName);
        traitMethodTypes[key] = TraitMethodEntry{
            string(traitName.str()), string(methodName.str()), funcType,
            std::move(paramNames)};
    }

    ValueTy *getMember(llvm::StringRef name) {
        auto it = members.find(name);
        if (it == members.end()) {
            return nullptr;
        }
        return &it->second;
    }

    AccessKind getMemberAccess(llvm::StringRef name) const {
        auto it = memberAccess.find(name);
        if (it == memberAccess.end()) {
            return AccessKind::GetOnly;
        }
        return it->second;
    }

    bool isEmbeddedMember(llvm::StringRef name) const {
        return embeddedMembers.contains(name);
    }

    FuncType *getMethodType(llvm::StringRef name) {
        auto it = methodTypes.find(name);
        if (it == methodTypes.end()) {
            return nullptr;
        }
        return it->second;
    }

    FuncType *getTraitMethodType(llvm::StringRef traitName,
                                 llvm::StringRef methodName) {
        return getTraitMethodTypeByKey(
            llvm::StringRef(traitMethodSlotKey(traitName, methodName)));
    }

    FuncType *getTraitMethodTypeByKey(llvm::StringRef key) {
        auto it = traitMethodTypes.find(key.str());
        if (it == traitMethodTypes.end()) {
            return nullptr;
        }
        return it->second.funcType;
    }

    const std::vector<string> *getMethodParamNames(llvm::StringRef name) const {
        auto it = methodParamNames.find(name);
        if (it == methodParamNames.end()) {
            return nullptr;
        }
        return &it->second;
    }

    const std::vector<string> *getTraitMethodParamNames(
        llvm::StringRef traitName, llvm::StringRef methodName) const {
        return getTraitMethodParamNamesByKey(
            llvm::StringRef(traitMethodSlotKey(traitName, methodName)));
    }

    const std::vector<string> *getTraitMethodParamNamesByKey(
        llvm::StringRef key) const {
        auto it = traitMethodTypes.find(key.str());
        if (it == traitMethodTypes.end()) {
            return nullptr;
        }
        return &it->second.paramNames;
    }

    const TraitMethodEntry *getTraitMethodEntryByKey(llvm::StringRef key) const {
        auto it = traitMethodTypes.find(key.str());
        if (it == traitMethodTypes.end()) {
            return nullptr;
        }
        return &it->second;
    }

    std::vector<const TraitMethodEntry *> findTraitMethodsByLocalName(
        llvm::StringRef methodName) const {
        std::vector<const TraitMethodEntry *> matches;
        for (const auto &entry : traitMethodTypes) {
            if (llvm::StringRef(entry.second.methodName.tochara(),
                                entry.second.methodName.size()) == methodName) {
                matches.push_back(&entry.second);
            }
        }
        return matches;
    }

    const llvm::StringMap<ValueTy> &getMembers() const { return members; }
    const llvm::StringMap<AccessKind> &getMemberAccesses() const {
        return memberAccess;
    }
    const llvm::StringSet<> &getEmbeddedMembers() const {
        return embeddedMembers;
    }
    const llvm::StringMap<FuncType *> &getMethodTypes() const {
        return methodTypes;
    }
    const std::unordered_map<std::string, TraitMethodEntry> &
    getTraitMethodTypes() const {
        return traitMethodTypes;
    }
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

    llvm::Type *buildLLVMType(TypeTable &types) override;

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
                name.append(itemType->full_name.tochara(),
                            itemType->full_name.size());
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
    TypeClass *retType = nullptr;
    AbiKind abiKind = AbiKind::Native;

public:
    static string buildName(
        const std::vector<TypeClass *> &argTypes, TypeClass *retType,
        const std::vector<BindingKind> &argBindingKinds = {},
        AbiKind abiKind = AbiKind::Native) {
        std::string funcTypeName = abiKind == AbiKind::C ? "fc" : "fn";
        if (retType) {
            funcTypeName += "_";
            funcTypeName.append(retType->full_name.tochara(),
                                retType->full_name.size());
        }
        for (std::size_t i = 0; i < argTypes.size(); ++i) {
            funcTypeName += ".";
            if (!argBindingKinds.empty() &&
                argBindingKinds[i] == BindingKind::Ref) {
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

    auto &getArgTypes() const { return argTypes; }
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

    FuncType(std::vector<TypeClass *> &&args, TypeClass *retType,
             string full_name, std::vector<BindingKind> argBindingKinds = {},
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

    llvm::Type *buildLLVMType(TypeTable &types) override;
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
                name.append(retType->full_name.tochara(),
                            retType->full_name.size());
            }
            name += ")";
            return string(name.c_str());
        }
        return pointeeType ? pointeeType->full_name + "*"
                           : string("<unknown>*");
    }

    PointerType(TypeClass *pointeeType)
        : TypeClass(buildName(pointeeType)), pointeeType(pointeeType) {}
    TypeClass *getPointeeType() { return pointeeType; }

    llvm::Type *buildLLVMType(TypeTable &types) override;
};

class IndexablePointerType : public TypeClass {
    TypeClass *elementType;

public:
    static string buildName(TypeClass *elementType) {
        return elementType ? elementType->full_name + "[*]"
                           : string("<unknown>[*]");
    }

    explicit IndexablePointerType(TypeClass *elementType)
        : TypeClass(buildName(elementType)), elementType(elementType) {}

    TypeClass *getElementType() { return elementType; }
    llvm::Type *buildLLVMType(TypeTable &types) override;
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
    llvm::Type *buildLLVMType(TypeTable &types) override;
};

TypeClass *
stripTopLevelConst(TypeClass *type);

template<typename T>
inline T *
asUnqualified(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    return storageType ? storageType->as<T>() : nullptr;
}

bool
isConstQualifiedType(TypeClass *type);
bool
isConstQualificationConvertible(TypeClass *targetType, TypeClass *sourceType);
bool
isConstQualificationConvertibleFromValue(TypeClass *targetType,
                                         TypeClass *sourceType);
TypeClass *
materializeValueType(TypeTable *typeTable, TypeClass *type);
bool
isFullyWritableValueType(TypeClass *type);
bool
isFullyWritableStructFieldType(TypeClass *type);

class TypeTable {
    struct TypeMap {
        TypeClass *type;   // main type
        TypeMap *pointer;  // main type's pointer type
        TypeMap *array;    // main type's array type
        TypeMap() : type(nullptr), pointer(nullptr), array(nullptr) {}
        TypeMap(TypeClass *type)
            : type(type), pointer(nullptr), array(nullptr) {}
    };

    static std::size_t nextInstanceId() {
        static std::size_t nextId = 1;
        return nextId++;
    }

    llvm::Module &module;
    std::size_t instanceId_;

    llvm::StringMap<TypeMap> typeMap;
    std::vector<TypeClass *> ownedTypes_;
    std::unordered_map<const TypeClass *, llvm::Type *> llvmTypes_;
    struct MethodBindingKey {
        const StructType *parent = nullptr;
        string name;

        bool operator==(const MethodBindingKey &other) const {
            return parent == other.parent && name == other.name;
        }
    };
    struct MethodBindingKeyHash {
        std::size_t operator()(const MethodBindingKey &key) const {
            return std::hash<const StructType *>{}(key.parent) ^
                   (std::hash<string>{}(key.name) << 1);
        }
    };
    std::unordered_map<MethodBindingKey, Function *, MethodBindingKeyHash>
        methodFunctions_;
    std::unordered_set<llvm::StructType *> materializingStructBodies_;
    std::unordered_set<const StructType *> interningStructContents_;
    std::unordered_set<const TypeClass *> retainedTypes_;

    void retainOwnedType(TypeClass *type) {
        if (!type) {
            return;
        }
        if (retainedTypes_.insert(type).second) {
            type->retain();
            ownedTypes_.push_back(type);
        }
    }

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
    TypeTable(llvm::Module &module)
        : module(module), instanceId_(nextInstanceId()) {}
    ~TypeTable() {
        for (auto *type : ownedTypes_) {
            type->release();
        }
    }

    llvm::LLVMContext &getContext() { return module.getContext(); }
    llvm::Module &getModule() { return module; }
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
            auto *llvmStruct =
                llvm::cast<llvm::StructType>(structType->buildLLVMType(*this));
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
        if (auto *base = type->as<BaseType>();
            base && base->type == BaseType::USIZE) {
            return static_cast<std::uint64_t>(
                module.getDataLayout().getPointerSize(0));
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
        type->typeSize =
            static_cast<int>(module.getDataLayout().getTypeAllocSize(llvmType));
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
        retainOwnedType(type);
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

    StructType *createOpaqueStructType(const ::string &fullName,
                                       StructDeclKind declKind =
                                           StructDeclKind::Native,
                                       string appliedTemplateName = {},
                                       std::vector<TypeClass *> appliedTypeArgs =
                                           {},
                                       const CompilationUnit *templateOwnerUnit =
                                           nullptr) {
        if (auto *type = getType(fullName)) {
            auto *structType = type->as<StructType>();
            if (structType) {
                structType->setDeclKind(declKind);
                if (!appliedTemplateName.empty()) {
                    structType->setAppliedTemplateInfo(
                        std::move(appliedTemplateName),
                        std::move(appliedTypeArgs),
                        templateOwnerUnit ? templateOwnerUnit
                                          : structType->getAppliedTemplateOwnerUnit());
                }
            }
            return structType;
        }
        auto *structType = new StructType(fullName, declKind,
                                          std::move(appliedTemplateName),
                                          std::move(appliedTypeArgs));
        if (templateOwnerUnit && structType->isAppliedTemplateInstance()) {
            structType->setAppliedTemplateInfo(
                structType->getAppliedTemplateName(),
                structType->getAppliedTypeArgs(), templateOwnerUnit);
        }
        addType(fullName, structType);
        return structType;
    }

    StructType *createOpaqueStructType(const std::string &fullName,
                                       StructDeclKind declKind =
                                           StructDeclKind::Native,
                                       string appliedTemplateName = {},
                                       std::vector<TypeClass *> appliedTypeArgs =
                                           {},
                                       const CompilationUnit *templateOwnerUnit =
                                           nullptr) {
        return createOpaqueStructType(string(fullName), declKind,
                                      std::move(appliedTemplateName),
                                      std::move(appliedTypeArgs),
                                      templateOwnerUnit);
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

    AnyType *createAnyType() {
        constexpr llvm::StringRef anyName("any");
        if (auto *type = getType(anyName)) {
            return dynamic_cast<AnyType *>(type);
        }
        auto *anyType = new AnyType();
        addType(anyName, anyType);
        return anyType;
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

    DynTraitType *createDynTraitType(const ::string &traitName,
                                     bool readOnlyDataPtr = false) {
        auto typeName = DynTraitType::buildName(traitName, readOnlyDataPtr);
        if (auto *type = getType(typeName)) {
            return type->as<DynTraitType>();
        }
        auto *dynType = new DynTraitType(traitName, readOnlyDataPtr);
        addType(typeName, dynType);
        return dynType;
    }

    DynTraitType *createDynTraitType(const std::string &traitName,
                                     bool readOnlyDataPtr = false) {
        return createDynTraitType(string(traitName), readOnlyDataPtr);
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

    FuncType *getOrCreateFunctionType(
        const std::vector<TypeClass *> &argTypes, TypeClass *retType,
        std::vector<BindingKind> argBindingKinds = {},
        AbiKind abiKind = AbiKind::Native) {
        if (!argBindingKinds.empty() &&
            argBindingKinds.size() != argTypes.size()) {
            return nullptr;
        }
        for (auto *argType : argTypes) {
            if (!argType) {
                return nullptr;
            }
        }
        auto funcTypeName =
            FuncType::buildName(argTypes, retType, argBindingKinds, abiKind);
        if (auto *existing = getType(funcTypeName)) {
            return existing->as<FuncType>();
        }
        auto *funcType =
            new FuncType(std::vector<TypeClass *>(argTypes), retType,
                         funcTypeName, std::move(argBindingKinds), abiKind);
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
        if (type->as<AnyType>()) {
            if (auto *existing = getType(type->full_name)) {
                return existing;
            }
            addType(type->full_name, type);
            return type;
        }
        if (auto *structType = type->as<StructType>()) {
            auto internStructContents =
                [&](StructType *targetStruct) -> StructType * {
                auto [_, inserted] =
                    interningStructContents_.insert(targetStruct);
                if (!inserted) {
                    return targetStruct;
                }

                struct Guard {
                    std::unordered_set<const StructType *> &active;
                    const StructType *type;
                    ~Guard() { active.erase(type); }
                } guard{interningStructContents_, targetStruct};

                // Applied/generic struct methods often carry a receiver that
                // points back to an equivalent struct instance from another
                // type table. Re-entering content interning for the canonical
                // target struct must reuse the in-progress placeholder.
                llvm::StringMap<StructType::ValueTy> internedMembers;
                llvm::StringMap<AccessKind> internedMemberAccess;
                llvm::StringSet<> internedEmbeddedMembers;
                bool reusedOriginalMembers = true;
                for (const auto &member : structType->getMembers()) {
                    auto *internedMemberType = internType(member.second.first);
                    if (!internedMemberType) {
                        return nullptr;
                    }
                    reusedOriginalMembers =
                        reusedOriginalMembers &&
                        internedMemberType == member.second.first;
                    internedMembers[member.first()] = {
                        internedMemberType, member.second.second};
                    internedMemberAccess[member.first()] =
                        structType->getMemberAccess(member.first());
                    if (structType->isEmbeddedMember(member.first())) {
                        internedEmbeddedMembers.insert(member.first());
                    }
                }

                llvm::StringMap<FuncType *> internedMethodTypes;
                llvm::StringMap<std::vector<string>> internedMethodParamNames;
                for (const auto &method : structType->getMethodTypes()) {
                    auto *internedMethod = internType(method.second);
                    auto *funcType = internedMethod ? internedMethod->as<FuncType>()
                                                   : nullptr;
                    if (!funcType) {
                        return nullptr;
                    }
                    internedMethodTypes[method.first()] = funcType;
                    if (const auto *paramNames =
                            structType->getMethodParamNames(method.first())) {
                        internedMethodParamNames[method.first()] = *paramNames;
                    }
                }

                std::vector<StructType::TraitMethodEntry> internedTraitMethods;
                internedTraitMethods.reserve(structType->getTraitMethodTypes().size());
                for (const auto &method : structType->getTraitMethodTypes()) {
                    auto *internedMethod = internType(method.second.funcType);
                    auto *funcType = internedMethod ? internedMethod->as<FuncType>()
                                                   : nullptr;
                    if (!funcType) {
                        return nullptr;
                    }
                    auto entry = method.second;
                    entry.funcType = funcType;
                    internedTraitMethods.push_back(std::move(entry));
                }

                targetStruct->setDeclKind(structType->getDeclKind());
                if (!targetStruct->isAppliedTemplateInstance() &&
                    structType->isAppliedTemplateInstance()) {
                    targetStruct->setAppliedTemplateInfo(
                        structType->getAppliedTemplateName(),
                        structType->getAppliedTypeArgs(),
                        structType->getAppliedTemplateOwnerUnit());
                }
                if (!structType->isOpaque()) {
                    targetStruct->complete(internedMembers, internedMemberAccess,
                                           internedEmbeddedMembers);
                }
                for (const auto &method : internedMethodTypes) {
                    if (!targetStruct->getMethodType(method.first()) ||
                        targetStruct->getMethodType(method.first()) !=
                            method.second) {
                        std::vector<string> paramNames;
                        auto foundParamNames =
                            internedMethodParamNames.find(method.first());
                        if (foundParamNames != internedMethodParamNames.end()) {
                            paramNames = foundParamNames->second;
                        }
                        targetStruct->addMethodType(method.first(),
                                                    method.second,
                                                    std::move(paramNames));
                    }
                }
                for (const auto &method : internedTraitMethods) {
                    auto key = traitMethodSlotKey(method.traitName,
                                                  method.methodName);
                    if (!targetStruct->getTraitMethodTypeByKey(key) ||
                        targetStruct->getTraitMethodTypeByKey(key) !=
                            method.funcType) {
                        targetStruct->addTraitMethodType(
                            llvm::StringRef(method.traitName.tochara(),
                                            method.traitName.size()),
                            llvm::StringRef(method.methodName.tochara(),
                                            method.methodName.size()),
                            method.funcType, method.paramNames);
                    }
                }
                return targetStruct;
            };

            if (auto *existing = getType(type->full_name)) {
                if (existing == type) {
                    return existing;
                }
                auto *existingStruct = existing->as<StructType>();
                if (!existingStruct) {
                    return existing;
                }
                return internStructContents(existingStruct);
            }

            addType(type->full_name, type);
            return internStructContents(structType);

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
        if (auto *dynTrait = type->as<DynTraitType>()) {
            if (auto *existing = getType(type->full_name)) {
                return existing;
            }
            addType(type->full_name, dynTrait);
            return dynTrait;
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
                reusedOriginalItems =
                    reusedOriginalItems && internedItem == item;
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

    void bindMethodFunction(StructType *parent, llvm::StringRef name,
                            Function *func) {
        methodFunctions_[{parent, string(name)}] = func;
    }
    Function *getMethodFunction(const StructType *parent,
                                llvm::StringRef name) const {
        auto found = methodFunctions_.find({parent, string(name)});
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

        if (dynamic_cast<AnyTypeNode *>(node)) {
            return createAnyType();
        }

        if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
            auto baseName = describeTypeNode(applied, "<unknown type>");
            return getType(llvm::StringRef(baseName));
        }

        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            if (base->hasSyntax()) {
                auto rawName = describeDotLikeSyntax(base->syntax);
                return getType(llvm::StringRef(rawName));
            }
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
            return elementType ? createIndexablePointerType(elementType)
                               : nullptr;
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

        if (auto *dynType = dynamic_cast<DynTypeNode *>(node)) {
            bool readOnlyDataPtr = false;
            auto *base = getDynTraitBaseNode(dynType, &readOnlyDataPtr);
            if (!base) {
                return nullptr;
            }
            auto rawName = base->hasSyntax()
                               ? describeDotLikeSyntax(base->syntax)
                               : toStdString(base->name);
            if (rawName.empty() || getType(llvm::StringRef(rawName))) {
                return nullptr;
            }
            return createDynTraitType(rawName, readOnlyDataPtr);
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
            auto *funcType = getOrCreateFunctionType(
                argTypes, retType, std::move(argBindingKinds), AbiKind::Native);
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
    return storageType && (storageType->as<PointerType>() ||
                           storageType->as<IndexablePointerType>());
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
        case BaseType::USIZE:
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
           isConstQualificationConvertibleFromValue(dstType, srcType);
}

inline DynTraitType *
getReadOnlyDynTraitType(TypeClass *type) {
    auto *dynType = asUnqualified<DynTraitType>(type);
    return dynType && dynType->hasReadOnlyDataPtr() ? dynType : nullptr;
}

}  // namespace lona
