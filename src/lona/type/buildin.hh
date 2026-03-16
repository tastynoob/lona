#pragma once

#include "../type/scope.hh"

namespace lona {

class IntType : public BaseType {

public:
    IntType(Type id, std::string name)
        : BaseType(id, string(name.c_str())) {}

    bool isSigned() {
        return type == I8 || type == I16 || type == I32 || type == I64;
    }

    void binaryOperation(llvm::IRBuilder<>& builder, Object* left,
                            token_type op, ObjectPtr right, ObjectPtr& res);
    void unaryOperation(llvm::IRBuilder<>& builder, token_type op,
                           ObjectPtr value, ObjectPtr& res);
};

class FLoatType : public BaseType {
public:
    FLoatType(Type id, std::string name)
        : BaseType(id, string(name.c_str())) {}
};

class BoolType : public BaseType {
public:
    BoolType() : BaseType(BOOL, "bool") {}

    // Object* binaryOperation(llvm::IRBuilder<>& builder, Object* left,
    //     token_type op, Object* right) override;
    // Object* unaryOperation(llvm::IRBuilder<>& builder, token_type op,
    //     Object* value) override;
};

void
initBuildinType(Scope* scope);

extern IntType* u8Ty;
extern IntType* i8Ty;
extern IntType* u16Ty;
extern IntType* i16Ty;
extern IntType* u32Ty;
extern IntType* i32Ty;
extern IntType* u64Ty;
extern IntType* i64Ty;
extern FLoatType* f32Ty;
extern FLoatType* f64Ty;
extern BoolType* boolTy;
extern PointerType* strTy;

}
