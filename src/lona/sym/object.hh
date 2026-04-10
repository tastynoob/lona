#pragma once

#include "lona/util/string.hh"
#include <any>
#include <cassert>
#include <iostream>
#include <llvm-18/llvm/IR/DerivedTypes.h>
#include <llvm-18/llvm/IR/Type.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include <string>
#include <sys/types.h>

namespace lona {

class AstFuncDecl;
class TypeClass;
class FuncType;
class Scope;
class BaseVar;
class CompilationUnit;

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
        REF_ALIAS = 1 << 3,
    };

    Object(TypeClass *type, uint32_t specifiers = EMPTY)
        : type(type), specifiers(specifiers) {}

    Object(llvm::Value *val, TypeClass *type, uint32_t specifiers = EMPTY)
        : val(val), type(type), specifiers(specifiers) {}

    template<class T>
    T *as() {
        return dynamic_cast<T *>(this);
    }

    // check if bind to llvm value
    bool isBindllvm() { return val != nullptr; }

    void createllvmValue(Scope *scope);

    void bindllvmValue(llvm::Value *val) {
        assert(isRegVal());
        this->val = val;
    }

    void setllvmValue(llvm::Value *val) { this->val = val; }

    void setType(TypeClass *newType) { this->type = newType; }

    uint32_t getSpecifiers() { return specifiers; }
    TypeClass *getType() { return type; }
    llvm::Value *getllvmValue() { return val; }
    bool isVariable() { return specifiers & VARIABLE; }
    bool isRegVal() { return specifiers & REG_VAL; }
    bool isReadOnly() { return specifiers & READONLY; }
    bool isRefAlias() { return specifiers & REF_ALIAS; }

    virtual llvm::Value *get(Scope *scope);
    virtual void set(Scope *scope, Object *src);
};

using ObjectPtr = Object *;

// i32, i64 ...
class BaseVar : public Object {
public:
    BaseVar(TypeClass *type, uint32_t specifiers = EMPTY)
        : Object(type, specifiers) {}
    BaseVar(llvm::Value *val, TypeClass *type, uint32_t specifiers = EMPTY)
        : Object(val, type, specifiers) {}
};

// only Basevar can be modified by const
class ConstVar : public BaseVar {
    std::any value;

public:
    ConstVar(TypeClass *type, std::any value)
        : BaseVar(type, Object::REG_VAL | Object::READONLY), value(value) {}

    const std::any &rawValue() const { return value; }
    llvm::Value *get(Scope *scope) override;
};

class PointerVar : public Object {
    ObjectPtr pointee;

public:
    PointerVar(ObjectPtr obj)
        : Object(obj->getType(), obj->getSpecifiers()), pointee(obj) {}
    void set(Scope *scope, Object *src) override { assert(false); }
    llvm::Value *get(Scope *scope) override { return val; }
};

class TupleVar : public Object {
public:
    TupleVar(TypeClass *type, uint32_t specifiers = EMPTY)
        : Object(type, specifiers) {}

    Object *getField(Scope *scope, const ::string &name);
};

class StructVar : public Object {
public:
    StructVar(TypeClass *type, uint32_t specifiers = EMPTY)
        : Object(type, specifiers) {}

    Object *getField(Scope *scope, const ::string &name);

    void set(Scope *scope, Object *src) override;
};

class ModuleObject : public Object {
    const CompilationUnit *unit_ = nullptr;

public:
    explicit ModuleObject(const CompilationUnit *unit)
        : Object(nullptr, Object::READONLY), unit_(unit) {}

    const CompilationUnit *unit() const { return unit_; }

    llvm::Value *get(Scope *scope) override;
    void set(Scope *scope, Object *src) override;
};

class TypeObject : public Object {
    TypeClass *declaredType_ = nullptr;

public:
    explicit TypeObject(TypeClass *declaredType)
        : Object(nullptr, Object::READONLY), declaredType_(declaredType) {}

    TypeClass *declaredType() const { return declaredType_; }

    llvm::Value *get(Scope *scope) override;
    void set(Scope *scope, Object *src) override;
};

}  // namespace lona
