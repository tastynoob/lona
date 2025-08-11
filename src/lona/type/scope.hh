#pragma once

#include "../obj/value.hh"
#include "../type/type.hh"
#include <llvm-18/llvm/IR/Value.h>
#include <stack>

namespace lona {

class FuncEnv;

class Scope {
protected:
    std::stack<ObjectPtr> opStack;
    llvm::StringMap<Object *> variables;
    Scope *parent = nullptr;

public:
    llvm::IRBuilder<> &builder;
    llvm::Module &module;

    Scope(llvm::IRBuilder<> &builder, llvm::Module &module)
        : builder(builder), module(module) {}
    Scope(Scope *parent)
        : builder(parent->builder), module(parent->module), parent(parent) {}

    ~Scope();

    virtual std::string getName() = 0;

    virtual llvm::Value *allocate(TypeClass *, bool t = false) { throw ""; };

    void addObj(llvm::StringRef name, Object *var);

    Object *getObj(llvm::StringRef name);

    void pushOp(ObjectPtr obj) {
        opStack.push(obj);
    }

    ObjectPtr popOp() {
        if (opStack.empty()) {
            throw "opStack is empty";
        }
        auto obj = opStack.top();
        opStack.pop();
        return obj;
    }
};

class GlobalScope : public Scope {
public:
    GlobalScope(llvm::IRBuilder<> &builder, llvm::Module &module)
        : Scope(builder, module) {}

    std::string getName() override { return module.getName().str(); }

    llvm::Value *allocate(TypeClass *type, bool is_extern = false) override;
};

class FuncScope : public Scope {
    friend class LocalScope;

    llvm::Instruction *alloc_point = nullptr;
    Object *ret_val = nullptr;
    llvm::BasicBlock *ret_block = nullptr;
    bool returned = false;

    int num_sub_scope = 0;
    int getNextScopeId() {
        return ++num_sub_scope;
    }

public:
    // used for struct body
    StructType *structTy = nullptr;

    FuncScope(FuncScope *parent)
        : Scope(parent),
          alloc_point(parent->alloc_point),
          ret_val(parent->ret_val),
          ret_block(parent->ret_block) {}
    FuncScope(GlobalScope *parent) : Scope(parent) {}

    void initRetVal(Object *ret_val) {
        assert(!this->ret_val);
        this->ret_val = ret_val;
    }
    Object *retVal() { return ret_val; }

    void initRetBlock(llvm::BasicBlock *ret_block) {
        assert(!this->ret_block);
        this->ret_block = ret_block;
    }
    llvm::BasicBlock *retBlock() { return ret_block; }

    void setReturned() { returned = true; }
    bool isReturned() { return returned; }

    std::string getName() override {
        return builder.GetInsertBlock()->getParent()->getName().str();
    }

    llvm::Value *allocate(TypeClass *type, bool is_temp = false) override;
};


class LocalScope : public Scope {
    FuncScope* const funcScope; // top
    std::string name;
public:
    LocalScope(FuncScope *func) : Scope(func), funcScope(func) {
        name = func->getName() + "." + std::to_string(func->getNextScopeId());
    }

    LocalScope(LocalScope *parent)
        : Scope(parent), funcScope(parent->funcScope) {
        name = funcScope->getName() + "." + std::to_string(funcScope->getNextScopeId());
    }

    llvm::Value *allocate(TypeClass *type, bool t = false) override { return funcScope->allocate(type, t); }

    std::string getName() override {
        return name;
    }
};

}