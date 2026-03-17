#pragma once

#include "object.hh"
#include <string>
#include <vector>


namespace lona {

class Function : public Object {
    std::vector<std::string> paramNames_;

public:
    Function(llvm::Function *val, FuncType *type,
             std::vector<std::string> paramNames = {})
        : Object((llvm::Function *)val, (TypeClass *)type),
          paramNames_(std::move(paramNames)) {}

    Object *call(Scope *scope, std::vector<Object *> &args);
    const std::vector<std::string> &paramNames() const { return paramNames_; }

    llvm::Value *get(Scope *scope) override { return val; }

    void set(Scope *scope, Object *src) override {
        throw "readonly literal value";
    }
};

Object *emitFunctionCall(Scope *scope, llvm::Value *calleeValue, FuncType *funcType,
                         std::vector<Object *> &args);


}
