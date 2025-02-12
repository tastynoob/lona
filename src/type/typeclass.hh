#pragma once

#include "ast/astnode.hh"
#include "codegen/base.hh"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

namespace lona {

typedef llvm::Value *(*TypeOps)(llvm::IRBuilder<> &builder, Object *left,
                                Object *right);

class Object;

class TypeClass {
    llvm::Type *llvmType;

public:
    TypeClass(llvm::Type *llvmType) : llvmType(llvmType) {}

    llvm::Type *getllvmType() { return llvmType; }

    virtual Object *newObj(llvm::Value *val);

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

    virtual Object *callOperation(llvm::IRBuilder<> &builder, Object *value,
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
    BaseType(llvm::Type *llvmType, Type type)
        : TypeClass(llvmType), type(type) {}
};

class StructType : public TypeClass {
    llvm::StringMap<std::pair<TypeClass *, int>> members;

public:
    StructType(llvm::Type *llvmType,
               llvm::StringMap<std::pair<TypeClass *, int>> &&members)
        : TypeClass(llvmType), members(members) {}

    Object *newObj(llvm::Value *val) override;

    Object *fieldSelect(llvm::IRBuilder<> &builder, Object *value,
                        const std::string &field) override;
};

// local type
class FuncType : public TypeClass {
    std::vector<TypeClass *> argTypes;
    TypeClass *retType;

public:
    FuncType(llvm::Type *llvmType, std::vector<TypeClass *> &&args,
             TypeClass *retType)
        : TypeClass(llvmType), argTypes(args), retType(retType) {}

    Object *newObj(llvm::Value *val) override { assert(false); }

    Object *callOperation(llvm::IRBuilder<> &builder, Object *value,
                          std::vector<Object *> args) override;
};

// local type, local type will not be add to typelist
class PointerType : public TypeClass {
    TypeClass *originalType;
    // multi level pointer/array support
    std::vector<uint64_t> pointerLevels;

public:
    // -1 is mean pointer
    PointerType(TypeClass *originalType,
                uint64_t array_size = pointerType_pointer)
        : TypeClass(originalType->getllvmType()->getPointerTo()),
          originalType(originalType) {
        assert(!originalType->getllvmType()->isPointerTy());
        pointerLevels.push_back(array_size);
    }
    TypeClass *getOriginalType() { return originalType; }
    bool is(TypeClass *t) override;
};

}  // namespace lona