#pragma once

#include "object.hh"
#include <string>
#include <vector>


namespace lona {

class Function : public Object {
    std::vector<string> paramNames_;
    bool hasImplicitSelf_ = false;

public:
    Function(llvm::Function *val, FuncType *type,
             std::vector<string> paramNames = {},
             bool hasImplicitSelf = false)
        : Object((llvm::Function *)val, (TypeClass *)type),
          paramNames_(std::move(paramNames)),
          hasImplicitSelf_(hasImplicitSelf) {}

    Object *call(Scope *scope, std::vector<Object *> &args);
    const std::vector<string> &paramNames() const { return paramNames_; }
    bool hasImplicitSelf() const { return hasImplicitSelf_; }

    llvm::Value *get(Scope *scope) override { return val; }

    void set(Scope *scope, Object *src) override {
        throw "readonly literal value";
    }
};

Object *emitFunctionCall(Scope *scope, llvm::Value *calleeValue, FuncType *funcType,
                         std::vector<Object *> &args,
                         bool hasImplicitSelf = false);


}
