#pragma once

#include "lona/sema/hir.hh"
#include "lona/sym/func.hh"
#include "lona/type/type.hh"

namespace lona {

inline FuncType *
getFunctionPointerTarget(TypeClass *type) {
    auto *pointeeType = getRawPointerPointeeType(type);
    return pointeeType ? pointeeType->as<FuncType>() : nullptr;
}

inline Function *
getDirectFunctionCallee(HIRExpr *callee) {
    auto *calleeValue = dynamic_cast<HIRValue *>(callee);
    auto *value = calleeValue ? calleeValue->getValue() : nullptr;
    return value ? value->as<Function>() : nullptr;
}

}  // namespace lona
