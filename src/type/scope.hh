#pragma once

#include "type/typeclass.hh"
#include "type/value.hh"

namespace lona {

class FuncEnv;

class Scope {
protected:
    llvm::IRBuilder<> &builder;
    llvm::Module &module;
    llvm::StringMap<TypeClass *> typeMap;
    llvm::StringMap<Object *> variables;

    Scope *parent = nullptr;

public:
    Scope(llvm::IRBuilder<> &builder, llvm::Module &module)
        : builder(builder), module(module) {}
    Scope(Scope *parent)
        : builder(parent->builder), module(parent->module), parent(parent) {}

    ~Scope();

    llvm::IRBuilder<> &getBuilder() { return builder; }

    llvm::Module &getModule() { return module; }

    virtual std::string getName() = 0;

    virtual Object *allocate(TypeClass *, bool t = false) = 0;

    void addObj(llvm::StringRef name, Object *var);

    Object *getObj(llvm::StringRef name);

    void addType(llvm::StringRef name, TypeClass *type);

    TypeClass *getType(llvm::StringRef name);

    TypeClass *getType(TypeHelper *const type);
};

class GlobalScope : public Scope {
public:
    GlobalScope(llvm::IRBuilder<> &builder, llvm::Module &module)
        : Scope(builder, module) {}

    std::string getName() override { return module.getName().str(); }

    Object *allocate(TypeClass *type, bool is_extern = false) override;
};

class FuncScope : public Scope {
    llvm::Instruction *alloc_point = nullptr;

public:
    FuncScope(llvm::IRBuilder<> &builder, llvm::Module &module)
        : Scope(builder, module) {}
    FuncScope(FuncScope *parent)
        : Scope(parent), alloc_point(parent->alloc_point) {}
    FuncScope(Scope *parent) : Scope(parent) {}

    std::string getName() override {
        return builder.GetInsertBlock()->getParent()->getName().str();
    }

    Object *allocate(TypeClass *type, bool is_temp = false) override;
};

// if, for block
typedef FuncScope LocalScope;

}