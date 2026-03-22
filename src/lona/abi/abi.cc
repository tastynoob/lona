#include "abi.hh"
#include "c_abi.hh"
#include "native_abi.hh"

namespace lona {

AbiFunctionSignature
classifyFunctionAbi(TypeTable &types, FuncType *funcType, bool hasImplicitSelf) {
    if (!funcType || funcType->getAbiKind() == AbiKind::Native) {
        return classifyNativeFunctionAbi(types, funcType, hasImplicitSelf);
    }
    return classifyCFunctionAbi(types, funcType, hasImplicitSelf);
}

llvm::FunctionType *
getFunctionAbiLLVMType(TypeTable &types, FuncType *funcType,
                       bool hasImplicitSelf) {
    return classifyFunctionAbi(types, funcType, hasImplicitSelf).llvmType;
}

}  // namespace lona
