#pragma once

#include "../ast/astnode.hh"
#include "../visitor.hh"
#include "../sym/object.hh"
#include "lona/type/llvmenv.hh"
#include <cassert>
#include <cstddef>
#include <llvm-18/llvm/ADT/ArrayRef.h>
#include <llvm-18/llvm/ADT/StringMap.h>
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
protected:
    llvm::Type * llvmType = nullptr;

public:
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

    // create opaque struct
    StructType(string full_name)
        : TypeClass(full_name), opaque(true) {}



    bool isOpaque() const { return opaque; }

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
          hasSROA(retType->shouldReturnByPointer()) {
        // if hasSroa, the first arg is the return value
    }
};

class PointerType : public TypeClass {
    TypeClass *pointeeType;

public:
    PointerType(TypeClass *pointeeType)
        : TypeClass(pointeeType->full_name),
          pointeeType(pointeeType) {
    }
    TypeClass *getPointeeType() { return pointeeType; }
};


class TypeTable {
    struct TypeMap {
        TypeClass* type; // main type
        TypeMap* pointer; // main type's pointer type
        TypeMap* array; // main type's array type
        TypeMap(TypeClass* type)
            : type(type), pointer(nullptr), array(nullptr) {}
    };

    llvm::Module& module;

    llvm::StringMap<TypeMap> typeMap;

    

public:
    TypeTable(llvm::Module& module) : module(module) {}

    llvm::LLVMContext& getContext() { return module.getContext(); }
    llvm::Module& getModule() { return module; }

    bool addType(llvm::StringRef name, TypeClass *type) {
        if (typeMap.find(name) != typeMap.end()) {
            return false;
        }
        typeMap[name] = type;
        return true;
    }

    TypeClass *getType(llvm::StringRef name) {
        auto it = typeMap.find(name);
        if (it == typeMap.end()) {
            return nullptr;
        }
        return it->second.type;
    }
};

}  // namespace lona