#pragma once

#include "abi.hh"

namespace lona {

AbiFunctionSignature
classifyCFunctionAbi(TypeTable &types, FuncType *funcType,
                     bool hasImplicitSelf = false);
llvm::FunctionType *
getCAbiFunctionType(TypeTable &types, FuncType *funcType,
                    bool hasImplicitSelf = false);

}  // namespace lona
