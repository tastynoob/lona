#pragma once

#include "type/scope.hh"

namespace lona {

class I32Type : public BaseType {
public:
    I32Type(llvm::Type* type) : BaseType(type, I32) {}
    Object* binaryOperation(llvm::IRBuilder<>& builder, Object* left,
                            token_type op, Object* right) override;
    Object* unaryOperation(llvm::IRBuilder<>& builder, token_type op,
                           Object* value) override;
    Object* assignOperation(llvm::IRBuilder<>& builder, Object* left,
                            Object* right) override;
};

class BoolType : public BaseType {
public:
    BoolType(llvm::Type* type) : BaseType(type, BOOL) {}
};

void
initBuildinType(Scope* scope);

extern I32Type* i32Ty;
extern BoolType* boolTy;

}