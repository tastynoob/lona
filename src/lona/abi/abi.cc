#include "abi.hh"
#include "c_abi.hh"
#include "native_abi.hh"
#include <llvm-18/llvm/IR/Function.h>
#include <llvm-18/llvm/IR/Metadata.h>

namespace lona {

namespace {

llvm::StringRef
functionAbiMetadataKey() {
    return "lona.abi.kind";
}

llvm::StringRef
functionAbiMetadataValue(AbiKind abiKind) {
    return abiKind == AbiKind::C ? "c" : "native";
}

}  // namespace

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

void
annotateFunctionAbi(llvm::Function &func, AbiKind abiKind) {
    auto &context = func.getContext();
    func.setMetadata(functionAbiMetadataKey(),
                     llvm::MDNode::get(
                         context,
                         llvm::MDString::get(context, functionAbiMetadataValue(abiKind))));
}

std::optional<AbiKind>
functionAbiAnnotation(const llvm::Function &func) {
    auto *node = func.getMetadata(functionAbiMetadataKey());
    if (!node || node->getNumOperands() != 1) {
        return std::nullopt;
    }
    auto *value = llvm::dyn_cast<llvm::MDString>(node->getOperand(0));
    if (!value) {
        return std::nullopt;
    }
    if (value->getString() == functionAbiMetadataValue(AbiKind::C)) {
        return AbiKind::C;
    }
    if (value->getString() == functionAbiMetadataValue(AbiKind::Native)) {
        return AbiKind::Native;
    }
    return std::nullopt;
}

}  // namespace lona
