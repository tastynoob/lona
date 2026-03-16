#pragma once

#include "../ast/astnode.hh"
#include "../visitor.hh"
#include "../sym/object.hh"
#include "lona/type/llvmenv.hh"
#include <cassert>
#include <cstddef>
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
#include <vector>

namespace lona {

class TypeClass;
class PointerType;
class StructType;

static const int RVO_THRESHOLD = 16;

class TypeClass {
public:
    llvm::Type * llvmType = nullptr;
    string const full_name;
    int typeSize;  // the size of the type in bytes

    TypeClass(string full_name)
        : full_name(full_name) {}

    template<typename T>
    T *as() {
        return dynamic_cast<T *>(this);
    }

    bool equal(TypeClass *t) { return this == t; }

    bool shouldReturnByPointer() {
        if (as<StructType>()) {
            return typeSize > RVO_THRESHOLD;
        }
        return false;
    }

    llvm::Type* getLLVMType() {
        if (!llvmType) panic("LLVM type not generated yet");
        return llvmType;
    }

    virtual llvm::Type* genLLVMType(Env& env) = 0;

    virtual ObjectPtr newObj(Env& env, uint32_t specifiers = Object::EMPTY);
    virtual ObjectPtr newObj(uint32_t specifiers = Object::EMPTY) {
        return new BaseVar(this, specifiers);
    }
};

class BaseType : public TypeClass {
public:
    enum Type { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, BOOL } type;
    BaseType(Type type, string full_name)
        : TypeClass(full_name), type(type) {}

    llvm::Type* genLLVMType(Env& env) override;
};

class StructType : public TypeClass {
public:
    // type : index
    using ValueTy = std::pair<TypeClass *, int>;

private:
    llvm::StringMap<ValueTy> members;
    llvm::StringMap<Function *> funcs;

    bool opaque = false;
public:
    StructType(llvm::StringMap<ValueTy> &&members,
               string full_name)
        : TypeClass(full_name), members(members), opaque(false) {}

    StructType(llvm::StructType *llvm_type, string full_name)
        : TypeClass(full_name), opaque(true) {
        llvmType = llvm_type;
    }

    // create opaque struct
    StructType(string full_name)
        : TypeClass(full_name), opaque(true) {}

    bool isOpaque() const { return opaque; }

    void complete(const llvm::StringMap<ValueTy> &newMembers, int size) {
        members = newMembers;
        typeSize = size;
        opaque = false;
    }

    void addFunc(llvm::StringRef name, Function *func) {
        funcs[name] = func;
    }

    ValueTy *getMember(llvm::StringRef name) {
        auto it = members.find(name);
        if (it == members.end()) {
            return nullptr;
        }
        return &it->second;
    }

    Function *getFunc(llvm::StringRef name) {
        auto it = funcs.find(name);
        if (it == funcs.end()) {
            return nullptr;
        }
        return it->second;
    }

    llvm::Type *genLLVMType(Env& env) override {
        if (!llvmType) {
            llvmType = llvm::StructType::create(env.getContext(), full_name.tochara());
        }
        return llvmType;
    }

    ObjectPtr newObj(uint32_t specifiers = Object::EMPTY) override {
        return new StructVar(this, specifiers);
    }
};

class FuncType : public TypeClass {
    std::vector<TypeClass *> argTypes;
    TypeClass * retType = nullptr;
    bool hasSROA = false;
public:
    bool SROA() const { return hasSROA; }
    auto& getArgTypes() const { return argTypes; }
    TypeClass *getRetType() const { return retType; }

    FuncType(std::vector<TypeClass *> &&args,
             TypeClass *retType, string full_name)
        : TypeClass(full_name),
          argTypes(args),
          retType(retType),
          hasSROA(retType && retType->shouldReturnByPointer()) {
        // if hasSroa, the first arg is the return value
    }

    FuncType(llvm::FunctionType *llvm_type,
             std::vector<TypeClass *> &&args,
             TypeClass *retType,
             string full_name,
             int typeSize)
        : TypeClass(full_name),
          argTypes(args),
          retType(retType),
          hasSROA(retType && retType->shouldReturnByPointer()) {
        llvmType = llvm_type;
        this->typeSize = typeSize;
    }

    llvm::Type *genLLVMType(Env& env) override {
        if (!llvmType) {
            std::vector<llvm::Type *> llvmArgTypes;
            llvmArgTypes.reserve(argTypes.size());
            for (auto *argType : argTypes) {
                llvmArgTypes.push_back(argType->llvmType ? argType->llvmType : argType->genLLVMType(env));
            }
            auto *llvmRetType = retType
                ? (retType->llvmType ? retType->llvmType : retType->genLLVMType(env))
                : llvm::Type::getVoidTy(env.getContext());
            llvmType = llvm::FunctionType::get(llvmRetType, llvmArgTypes, false);
        }
        return llvmType;
    }
};

class PointerType : public TypeClass {
    TypeClass *pointeeType;

public:
    PointerType(TypeClass *pointeeType)
        : TypeClass(pointeeType->full_name + "*"),
          pointeeType(pointeeType) {
    }
    TypeClass *getPointeeType() { return pointeeType; }

    llvm::Type *genLLVMType(Env& env) override {
        if (!llvmType) {
            auto *llvmPointeeType = pointeeType->llvmType
                ? pointeeType->llvmType
                : pointeeType->genLLVMType(env);
            llvmType = llvm::PointerType::getUnqual(llvmPointeeType);
        }
        return llvmType;
    }
};

class ArrayType : public TypeClass {
    TypeClass *elementType;
    std::vector<AstNode *> dimensions;

public:
    static string buildName(TypeClass *elementType,
                            const std::vector<AstNode *> &dimensions) {
        string name = elementType->full_name + "[";
        for (size_t i = 0; i < dimensions.size(); ++i) {
            if (i != 0) {
                name += ",";
            }
            if (dimensions[i] != nullptr) {
                name += "?";
            }
        }
        name += "]";
        return name;
    }

    ArrayType(TypeClass *elementType, std::vector<AstNode *> dimensions = {})
        : TypeClass(buildName(elementType, dimensions)),
          elementType(elementType),
          dimensions(std::move(dimensions)) {
    }

    TypeClass *getElementType() { return elementType; }
    const std::vector<AstNode *> &getDimensions() const { return dimensions; }

    llvm::Type *genLLVMType(Env& env) override {
        if (!llvmType) {
            auto *llvmElementType = elementType->llvmType
                ? elementType->llvmType
                : elementType->genLLVMType(env);
            llvmType = llvm::PointerType::getUnqual(llvmElementType);
            typeSize = sizeof(void *);
        }
        return llvmType;
    }
};



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

    

public:
    TypeTable(llvm::Module& module)
        : module(module), instanceId_(nextInstanceId()) {}

    llvm::LLVMContext& getContext() { return module.getContext(); }
    llvm::Module& getModule() { return module; }
    std::size_t instanceId() const { return instanceId_; }

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
        auto pointerName = pointeeType->full_name + "*";
        if (auto *type = getType(pointerName)) {
            return type->as<PointerType>();
        }
        auto *pointerType = new PointerType(pointeeType);
        pointerType->llvmType = llvm::PointerType::getUnqual(pointeeType->llvmType);
        pointerType->typeSize = sizeof(void *);
        addType(pointerName, pointerType);
        return pointerType;
    }

    ArrayType *createArrayType(TypeClass *elementType,
                               std::vector<AstNode *> dimensions = {}) {
        string arrayName = ArrayType::buildName(elementType, dimensions);
        if (auto *type = getType(arrayName)) {
            return type->as<ArrayType>();
        }

        auto *arrayType = new ArrayType(elementType, std::move(dimensions));
        arrayType->llvmType = llvm::PointerType::getUnqual(elementType->getLLVMType());
        arrayType->typeSize = sizeof(void *);
        addType(arrayName, arrayType);
        return arrayType;
    }

    FuncType *getOrCreateFunctionType(const std::vector<TypeClass *> &argTypes,
                                      TypeClass *retType) {
        std::vector<llvm::Type *> llvmArgTypes;
        llvmArgTypes.reserve(argTypes.size());
        string funcTypeName = "f";
        if (retType) {
            funcTypeName += "_";
            funcTypeName += retType->full_name;
        }
        for (auto *argType : argTypes) {
            if (!argType) {
                return nullptr;
            }
            llvmArgTypes.push_back(argType->llvmType);
            funcTypeName += ".";
            funcTypeName += argType->full_name;
        }
        if (auto *existing = getType(funcTypeName)) {
            return existing->as<FuncType>();
        }
        auto *llvmRetType =
            retType ? retType->llvmType : llvm::Type::getVoidTy(module.getContext());
        auto *llvmFuncType = llvm::FunctionType::get(llvmRetType, llvmArgTypes, false);
        auto *funcType = new FuncType(std::vector<TypeClass *>(argTypes), retType,
                                      funcTypeName);
        funcType->llvmType = llvmFuncType;
        funcType->typeSize = 0;
        addType(funcTypeName, funcType);
        return funcType;
    }

    TypeClass *getType(TypeNode *node) {
        if (!node) {
            return nullptr;
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

        if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
            auto *elementType = getType(array->base);
            if (!elementType) {
                return nullptr;
            }
            return createArrayType(elementType, array->dim);
        }

        if (auto *func = dynamic_cast<FuncTypeNode *>(node)) {
            std::vector<TypeClass *> argTypes;
            auto *retType = getType(func->ret);
            for (auto *arg : func->args) {
                auto *argType = getType(arg);
                if (!argType) {
                    return nullptr;
                }
                argTypes.push_back(argType);
            }
            return getOrCreateFunctionType(argTypes, retType);
        }

        return nullptr;
    }
};

}  // namespace lona
