#pragma once

#include "ast/astnode.hh"
#include "llvm.hh"
#include "visitor/base.hh"

namespace lona {

typedef llvm::Value *(*TypeOps)(llvm::IRBuilder<> &builder, BaseVariable* left, BaseVariable* right);

class BaseVariable;

class TypeClass {
    llvm::Type *llvmType;

public:
    TypeClass(llvm::Type *llvmType) : llvmType(llvmType) {}
    llvm::Type *getllvmType() { return llvmType; }
    virtual bool is(TypeClass *t) { return this == t; }
    virtual BaseVariable* binaryOperation(llvm::IRBuilder<> &builder, BaseVariable* left, token_type op, BaseVariable* right) = 0;
};

class BaseType : public TypeClass {
public:
    enum Type { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, BOOL } type;
    BaseType(llvm::Type *llvmType, Type type)
        : TypeClass(llvmType), type(type) {}
};

class FuncType : public TypeClass {
    std::vector<TypeClass *> args;
    TypeClass *retType;
};

class CompositeType : public TypeClass {
    llvm::StringMap<TypeClass *> fields;
};

// local type, local type will not be add to typelist
class PointerType : public TypeClass {
    TypeClass *originalType;
    // multi level pointer/array support
    std::vector<uint64_t> pointerLevels;

public:
    // -1 is mean pointer
    PointerType(TypeClass *originalType, uint64_t array_size = -1)
        : TypeClass(originalType->getllvmType()->getPointerTo()),
          originalType(originalType) {
        assert(!originalType->getllvmType()->isPointerTy());
        pointerLevels.push_back(array_size);
    }
    TypeClass *getOriginalType() { return originalType; }
    bool is(TypeClass *t) override;
};

class TypeManger {
    llvm::IRBuilder<> &builder;
    llvm::StringMap<TypeClass *> typeMap;

public:
    TypeManger(llvm::IRBuilder<> &builder);
    void addTypeClass(std::string &full_typename, TypeClass *type);
    TypeClass *getTypeClass(std::string *const full_typename);
    TypeClass *getTypeClass(std::string const full_typename);
    TypeClass *getTypeClass(TypeHelper *const type);
    TypeClass *getPointerType(TypeClass *originalType,
                              std::vector<uint64_t> &pointerLevels);
    TypeClass *getTupleOrStructType(std::vector<TypeClass *> &fields);
};

}  // namespace lona