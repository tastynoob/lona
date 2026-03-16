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

    llvm::Value *get(Scope *scope) override { return val; }

    void set(Scope *scope, Object *src) override {
        throw "readonly literal value";
    }
};

Object *emitFunctionCall(Scope *scope, llvm::Value *calleeValue, FuncType *funcType,
                         std::vector<Object *> &args);


}
