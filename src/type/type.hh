#pragma once

#include "ast/astnode.hh"
#include "codegen/base.hh"
#include "obj/value.hh"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

namespace lona {

typedef llvm::Value *(*TypeOps)(llvm::IRBuilder<> &builder, Object *left,
                                Object *right);

class Object;
class PointerType;

class TypeClass {
public:
    llvm::Type *const llvmType;
    std::string const full_name;
    int typeSize;

    TypeClass(llvm::Type *llvmType, std::string full_name, int typeSize)
        : llvmType(llvmType), full_name(full_name), typeSize(typeSize) {}

    template<typename T>
    bool is() {
        return dynamic_cast<const T *>(this) != nullptr;
    }

    // func parameters pass pointerization
    bool isPassByPointer() { return typeSize > 999; }

    virtual Object *newObj(llvm::Value *val,
                           uint32_t specifiers = Object::EMPTY);

    virtual PointerType *getPointerType(Scope *scope);

    virtual bool is(TypeClass *t) { return this == t; }

    virtual Object *binaryOperation(llvm::IRBuilder<> &builder, Object *left,
                                    token_type op, Object *right) {
        return nullptr;
    }

    virtual Object *unaryOperation(llvm::IRBuilder<> &builder, token_type op,
                                   Object *value) {
        return nullptr;
    }

    virtual Object *assignOperation(llvm::IRBuilder<> &builder, Object *left,
                                    Object *right) {
        return nullptr;
    }

    virtual Object *callOperation(Scope *scope, Object *value,
                                  std::vector<Object *> args) {
        return nullptr;
    }

    virtual Object *fieldSelect(llvm::IRBuilder<> &builder, Object *value,
                                const std::string &field) {
        return nullptr;
    }
};

class BaseType : public TypeClass {
public:
    enum Type { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, BOOL } type;
    BaseType(llvm::Type *llvmType, Type type, std::string full_name,
             int typeSize)
        : TypeClass(llvmType, full_name, typeSize), type(type) {}
};

class StructType : public TypeClass {
    llvm::StringMap<std::pair<TypeClass *, int>> members;
    llvm::StringMap<Method*> funcs;
public:
    StructType(llvm::Type *llvmType,
               llvm::StringMap<std::pair<TypeClass *, int>> &&members,
               std::string full_name, int typeSize)
        : TypeClass(llvmType, full_name, typeSize), members(members) {}

    StructType(llvm::Type *llvmType, std::string full_name)
        : TypeClass(llvmType, full_name, 0) {}

    TypeClass *getMember(const std::string &name) {
        auto it = members.find(name);
        if (it == members.end()) {
            return nullptr;
        }
        return it->second.first;
    }

    Method *getFunc(const std::string &name) {
        auto it = funcs.find(name);
        if (it == funcs.end()) {
            return nullptr;
        }
        return it->second;
    }

    void setMembers(llvm::StringMap<std::pair<TypeClass *, int>> &&members) {
        this->members = members;
    }

    void setFuncs(llvm::StringMap<Method*> &&funcs) {
        this->funcs = funcs;
    }

    Object *newObj(llvm::Value *val,
                   uint32_t specifiers = Object::EMPTY) override;

    Object *fieldSelect(llvm::IRBuilder<> &builder, Object *value,
                        const std::string &field) override;
};

// local type
class FuncType : public TypeClass {
    std::vector<TypeClass *> argTypes;
    TypeClass *retType;

public:
    FuncType(llvm::Type *llvmType, std::vector<TypeClass *> &&args,
             TypeClass *retType, std::string full_name, int typeSize)
        : TypeClass(llvmType, full_name, typeSize),
          argTypes(args),
          retType(retType) {}

    Object *newObj(llvm::Value *val,
                   uint32_t specifiers = Object::EMPTY) override {
        assert(false);
    }

    Object *callOperation(Scope *scope, Object *value,
                          std::vector<Object *> args) override;
};

// local type, local type will not be add to typelist
class PointerType : public TypeClass {
    TypeClass *originalType;
    // multi level pointer/array support
    std::vector<uint64_t> pointerLevels;

public:
    PointerType(TypeClass *originalType, uint64_t array_size, int typesize)
        : TypeClass(originalType->llvmType->getPointerTo(),
                    originalType->full_name + "*", typesize),
          originalType(originalType) {
        assert(!originalType->llvmType->isPointerTy());
        pointerLevels.push_back(array_size);
    }
    TypeClass *getOriginalType() { return originalType; }
    bool is(TypeClass *t) override;
};

}  // namespace lona