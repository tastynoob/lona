#pragma once

#include "../type/scope.hh"

namespace lona {

class IntType : public BaseType {

public:
    IntType(llvm::Type* type, Type id, std::string name)
        : BaseType(type, id, name, type->getIntegerBitWidth() / 8) {}

    bool isSigned() {
        return type == I8 || type == I16 || type == I32 || type == I64;
    }

    void binaryOperation(llvm::IRBuilder<>& builder, Object* left,
                            token_type op, ObjectPtr right, ObjectPtr& res) override;
    void unaryOperation(llvm::IRBuilder<>& builder, token_type op,
                           ObjectPtr value, ObjectPtr& res) override;
};

class FLoatType : public BaseType {
public:
    FLoatType(llvm::Type* type, Type id, std::string name)
        : BaseType(type, id, name, type->getPrimitiveSizeInBits() / 8) {}
};

class BoolType : public BaseType {
public:
    BoolType(llvm::Type* type) : BaseType(type, BOOL, "bool", 1) {}

    // Object* binaryOperation(llvm::IRBuilder<>& builder, Object* left,
    //     token_type op, Object* right) override;
    // Object* unaryOperation(llvm::IRBuilder<>& builder, token_type op,
    //     Object* value) override;
};

void
initBuildinType(Scope* scope);

extern IntType* i8Ty;
extern IntType* i16Ty;
extern IntType* i32Ty;
extern IntType* i64Ty;
extern BoolType* boolTy;

}