#pragma once

#include <any>
#include <iostream>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include <sys/types.h>

namespace lona {

class AstFuncDecl;
class TypeClass;
class FuncType;
class Scope;
class BaseVar;

class Object {
protected:
    TypeClass *type;
    llvm::Value *val = nullptr;
    uint32_t specifiers;

public:
    enum Specifier : uint32_t {
        EMPTY = 0,
        VARIABLE = 1 << 0,
        REG_VAL = 1 << 1,  // only for base type and small struct
        READONLY = 1 << 2,
    };

    Object(TypeClass* type, uint32_t specifiers = EMPTY)
        : type(type), specifiers(specifiers) {}

    Object(llvm::Value *val, TypeClass *type, uint32_t specifiers = EMPTY)
        : val(val), type(type), specifiers(specifiers) {}

    template<class T>
    T *as() {
        return dynamic_cast<T *>(this);
    }

    // check if bind to llvm value
    bool isBindllvm() {
        return val != nullptr;
    }

    void bindllvmValue(llvm::Value *val) {
        this->val = val;
    }

    uint32_t getSpecifiers() { return specifiers; }
    TypeClass *getType() { return type; }
    llvm::Value *getllvmValue() { return val; }
    bool isVariable() { return specifiers & VARIABLE; }
    bool isRegVal() { return specifiers & REG_VAL; }
    bool isReadOnly() { return specifiers & READONLY; }

    virtual llvm::Value *get(llvm::IRBuilder<> &builder);
    virtual void set(llvm::IRBuilder<> &builder, Object *src);
};

// i32, i64 ...
class BaseVar : public Object {
public:
    BaseVar(TypeClass *type, uint32_t specifiers = EMPTY)
        : Object(type, specifiers) {}
};

// only Basevar can be modified by const
class ConstVar : public BaseVar {
    std::any value;
public:
    ConstVar(TypeClass *type, std::any value)
        : BaseVar(type, Object::REG_VAL | Object::READONLY), value(value) {}
};

class PointerVar : public Object {
public:
    PointerVar(Object *obj)
        : Object(obj->getType(), obj->getSpecifiers()) {}
    void set(llvm::IRBuilder<> &builder, Object *src) override {
        assert(false);
    }
    llvm::Value *get(llvm::IRBuilder<> &builder) override { return val; }
};

class StructVar : public Object {
public:
    StructVar(TypeClass *type, uint32_t specifiers = EMPTY)
        : Object(type, specifiers) {}

    Object *getField(llvm::IRBuilder<> &builder, std::string name);

    void set(llvm::IRBuilder<> &builder, Object *src) override;
};

class Function : public Object {
    AstFuncDecl *funcDecl;

public:
    Function(llvm::Function *val, FuncType *type)
        : Object((llvm::Function *)val, (TypeClass *)type) {}

    Object *call(Scope *scope, std::vector<Object *> &args);

    llvm::Value *get(llvm::IRBuilder<> &builder) override { return val; }

    void set(llvm::IRBuilder<> &builder, Object *src) override {
        throw "readonly literal value";
    }
};


using ObjectPtr = Object *;

}  // namespace lona