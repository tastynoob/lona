#pragma once

#include "object.hh"
#include <vector>


namespace lona {

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

Object *emitFunctionCall(Scope *scope, llvm::Value *calleeValue, FuncType *funcType,
                         std::vector<Object *> &args);


}
