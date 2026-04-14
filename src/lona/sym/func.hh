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
             std::vector<string> paramNames = {}, bool hasImplicitSelf = false)
        : Object((llvm::Function *)val, (TypeClass *)type),
          paramNames_(std::move(paramNames)),
          hasImplicitSelf_(hasImplicitSelf) {}

    ObjectPtr call(Scope *scope, const std::vector<ObjectPtr> &args);
    const std::vector<string> &paramNames() const { return paramNames_; }
    bool hasImplicitSelf() const { return hasImplicitSelf_; }

    llvm::Value *get(Scope *scope) override { return val; }

    void set(Scope *scope, Object *src) override {
        throw "readonly literal value";
    }
};

ObjectPtr
emitFunctionCall(Scope *scope, llvm::Value *calleeValue, FuncType *funcType,
                 const std::vector<ObjectPtr> &args,
                 bool hasImplicitSelf = false);

}
