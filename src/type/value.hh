#pragma once

#include <iostream>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

namespace lona {

class AstFuncDecl;
class TypeClass;
class Variable;

class Object {
protected:
    TypeClass *type;
    llvm::Value *val;
    uint32_t specifiers;

public:
    enum Specifier : uint32_t {
        EMPTY = 0,
        VARIABLE = 1 << 0,
        READ_NO_LOAD = 1 << 1,
    };
    Object(llvm::Value *val, TypeClass *type, uint32_t specifiers = EMPTY)
        : val(val), type(type), specifiers(specifiers) {}
    TypeClass *getType() { return type; }
    llvm::Value *getllvmValue() { return val; }
    bool isVariable() { return specifiers & VARIABLE; }
    bool isReadNoLoad() { return specifiers & READ_NO_LOAD; }

    virtual llvm::Value *read(llvm::IRBuilder<> &builder);
    virtual void write(llvm::IRBuilder<> &builder, Object *src);
};

class RValue : public Object {
public:
    RValue(llvm::Value *val, TypeClass *type, uint32_t specifiers)
        : Object(val, type, specifiers) {}
    void write(llvm::IRBuilder<> &builder, Object *src) override {
        throw "readonly literal value";
    };
};

class Variable : public Object {
public:
    Variable(llvm::Value *val, TypeClass *type, uint32_t specifiers = EMPTY)
        : Object(val, type, specifiers | VARIABLE) {}
};

class StructVar : public Object {
public:
    StructVar(llvm::Value *val, TypeClass *type, uint32_t specifiers = EMPTY)
        : Object(val, type, specifiers | READ_NO_LOAD) {}
    llvm::Value *read(llvm::IRBuilder<> &builder) override { return val; }
    void write(llvm::IRBuilder<> &builder, Object *src) override;
};

class Functional : public Object {
    AstFuncDecl *funcDecl;

public:
    Functional(llvm::Function *val, TypeClass *type) : Object(val, type) {}

    llvm::Value *read(llvm::IRBuilder<> &builder) override { return val; }

    void write(llvm::IRBuilder<> &builder, Object *src) override {
        throw "readonly literal value";
    }
};

}  // namespace lona