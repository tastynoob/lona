#pragma once

#include "ast/astnode.hh"
#include "llvm.hh"

namespace lona {

typedef llvm::Value *(*TypeOps)(llvm::Value *left, llvm::Value *right);


class TypeClass {
    llvm::Type *llvmType;

public:
    enum PointerType { NON, POINTER, ARRAY };
    TypeClass(llvm::Type *llvmType) : llvmType(llvmType) {}
    llvm::Type *getllvmType() { return llvmType; }
    virtual llvm::Value *binaryOper(SymbolTable symbol, llvm::Value *right) = 0;
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
        F32,
        F64,
        BOOL
    } type;
    BaseType(llvm::Type *llvmType, Type type)
        : TypeClass(llvmType), type(type) {}
    llvm::Value *binaryOper(SymbolTable symbol, llvm::Value *right) override;
};

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
    llvm::Value *binaryOper(SymbolTable symbol, llvm::Value *right) override { return nullptr; };
};

class CompositeType : public TypeClass {
    llvm::StringMap<TypeClass *> fields;

public:
    enum Type { TUPLE, STRUCT };
};

class TypeManger {
    llvm::IRBuilder<> &builder;
    llvm::StringMap<TypeClass *> typeMap;

public:
    TypeManger(llvm::IRBuilder<> &builder);
    void addTypeClass(std::string &full_typename, TypeClass *type);
    TypeClass *getTypeClass(std::string &full_typename);
    TypeClass *getTypeClass(std::string const full_typename);
    TypeClass* getPointerType(TypeClass *originalType, std::vector<uint64_t>& pointerLevels);
    TypeClass* getTupleOrStructType(std::vector<TypeClass*>& fields);
};

}  // namespace lona