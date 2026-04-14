#pragma once

#include "../sym/object.hh"
#include "../type/type.hh"
#include <llvm-18/llvm/IR/Value.h>
#include <stack>

namespace lona {

class FuncEnv;
class GenericInstanceRegistry;

class Scope {
protected:
    std::stack<ObjectPtr> opStack;
    llvm::StringMap<ObjectPtr> variables;
    Scope *parent = nullptr;
    TypeTable *typeTable = nullptr;

public:
    llvm::IRBuilder<> &builder;
    llvm::Module &module;

    Scope(llvm::IRBuilder<> &builder, llvm::Module &module)
        : builder(builder), module(module) {}
    Scope(Scope *parent)
        : builder(parent->builder),
          module(parent->module),
          parent(parent),
          typeTable(parent->typeTable) {}

    virtual ~Scope() = default;

    virtual std::string getName() = 0;
    virtual llvm::Value *allocate(TypeClass *, bool t = false) { throw ""; };

    void setTypeTable(TypeTable *table) { typeTable = table; }
    TypeTable *types() const { return typeTable; }
    llvm::Type *getLLVMType(TypeClass *type) const;
    llvm::FunctionType *getLLVMFunctionType(FuncType *type) const;
    void bindMethodFunction(StructType *parent, llvm::StringRef name,
                            Function *func);
    Function *getMethodFunction(const StructType *parent,
                                llvm::StringRef name) const;
    void addObj(llvm::StringRef name, ObjectPtr var);
    void addObj(const ::string &name, ObjectPtr var) {
        addObj(llvm::StringRef(name.tochara(), name.size()), var);
    }

    bool hasLocalObj(llvm::StringRef name) const;
    bool hasLocalObj(const ::string &name) const {
        return hasLocalObj(llvm::StringRef(name.tochara(), name.size()));
    }

    Object *getObj(llvm::StringRef name);
    Object *getObj(const ::string &name) {
        return getObj(llvm::StringRef(name.tochara(), name.size()));
    }

    void pushOp(ObjectPtr obj) { opStack.push(obj); }

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
    GenericInstanceRegistry *genericInstanceRegistry_ = nullptr;

public:
    GlobalScope(llvm::IRBuilder<> &builder, llvm::Module &module)
        : Scope(builder, module) {}

    std::string getName() override { return module.getName().str(); }

    void setGenericInstanceRegistry(GenericInstanceRegistry *registry) {
        genericInstanceRegistry_ = registry;
    }
    GenericInstanceRegistry *genericInstanceRegistry() const {
        return genericInstanceRegistry_;
    }

    llvm::Value *allocate(TypeClass *type, bool is_extern = false) override;
};

class FuncScope : public Scope {
    friend class LocalScope;

    llvm::Instruction *alloc_point = nullptr;
    ObjectPtr ret_val;
    llvm::BasicBlock *ret_block = nullptr;
    bool returned = false;

    int num_sub_scope = 0;
    int getNextScopeId() { return ++num_sub_scope; }

public:
    // used for struct body
    StructType *structTy = nullptr;

    FuncScope(FuncScope *parent)
        : Scope(parent),
          alloc_point(parent->alloc_point),
          ret_val(parent->ret_val),
          ret_block(parent->ret_block) {}
    FuncScope(GlobalScope *parent) : Scope(parent) {}

    void initRetVal(ObjectPtr ret_val) {
        assert(!this->ret_val);
        this->ret_val = std::move(ret_val);
    }
    Object *retVal() { return ret_val.get(); }
    const ObjectPtr &retValObject() const { return ret_val; }

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
    FuncScope *const funcScope;  // top
    std::string name;

public:
    LocalScope(FuncScope *func) : Scope(func), funcScope(func) {
        name = func->getName() + "." + std::to_string(func->getNextScopeId());
    }

    LocalScope(LocalScope *parent)
        : Scope(parent), funcScope(parent->funcScope) {
        name = funcScope->getName() + "." +
               std::to_string(funcScope->getNextScopeId());
    }

    llvm::Value *allocate(TypeClass *type, bool t = false) override {
        return funcScope->allocate(type, t);
    }

    std::string getName() override { return name; }
};

}
