#pragma once

#include "obj/value.hh"
#include "type/type.hh"

namespace lona {

class FuncEnv;

class Scope {
protected:
    llvm::StringMap<TypeClass *> typeMap;
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

    virtual Object *allocate(TypeClass *, bool t = false) { throw ""; };

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
    friend class LocalScope;

    bool const func_root = false;
    llvm::Instruction *alloc_point = nullptr;
    Object *ret_val = nullptr;
    llvm::BasicBlock *ret_block = nullptr;
    bool returned = false;

    int num_sub_scope = 0;
    void accNumScope() {
        if (func_root)
            num_sub_scope++;
        else
            ((FuncScope *)parent)->accNumScope();
    }
    int getNumScope() {
        return func_root ? num_sub_scope : ((FuncScope *)parent)->getNumScope();
    }

public:
    // used for struct body
    StructType *structTy = nullptr;

    FuncScope(FuncScope *parent)
        : Scope(parent),
          alloc_point(parent->alloc_point),
          ret_val(parent->ret_val),
          ret_block(parent->ret_block) {}
    FuncScope(GlobalScope *parent) : Scope(parent), func_root(true) {}

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

    Object *allocate(TypeClass *type, bool is_temp = false) override;
};

// if, for block
class LocalScope : public FuncScope {
public:
    LocalScope(FuncScope *parent) : FuncScope(parent) { parent->accNumScope(); }
    std::string getName() override {
        return builder.GetInsertBlock()->getParent()->getName().str() + "." +
               std::to_string(getNumScope());
    }
};

}