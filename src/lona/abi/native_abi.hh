#pragma once

#include "abi.hh"

namespace lona {

bool isNativeAbiAggregateType(TypeClass *type);
bool usesNativeAbiPackedRegisterAggregate(TypeTable &types, TypeClass *type);
bool usesNativeAbiIndirectResult(TypeTable &types, TypeClass *type);
llvm::Type *getNativeAbiDirectLLVMType(TypeTable &types, TypeClass *type);
llvm::Value *packNativeAbiDirectValue(llvm::IRBuilder<> &builder, TypeTable &types,
                                      TypeClass *type, llvm::Value *value);
llvm::Value *loadNativeAbiDirectValue(llvm::IRBuilder<> &builder, TypeTable &types,
                                      TypeClass *type, llvm::Value *sourcePtr);
void storeNativeAbiDirectValue(llvm::IRBuilder<> &builder, TypeTable &types,
                               TypeClass *type, llvm::Value *value,
                               llvm::Value *destPtr);
AbiFunctionSignature classifyNativeFunctionAbi(TypeTable &types,
                                               FuncType *funcType,
                                               bool hasImplicitSelf = false);
llvm::FunctionType *getNativeAbiFunctionType(TypeTable &types, FuncType *funcType,
                                             bool hasImplicitSelf = false);

}  // namespace lona
