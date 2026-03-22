#pragma once

#include "../type/type.hh"
#include <cstddef>
#include <vector>

namespace lona {

enum class NativeAbiPassKind {
    Direct,
    IndirectValue,
    IndirectRef,
};

struct NativeAbiValueInfo {
    NativeAbiPassKind passKind = NativeAbiPassKind::Direct;
    llvm::Type *llvmType = nullptr;
    bool packedRegisterAggregate = false;
};

struct NativeAbiFunctionSignature {
    FuncType *sourceType = nullptr;
    bool hasImplicitSelf = false;
    bool hasIndirectResult = false;
    bool hasDirectAggregateResult = false;
    NativeAbiValueInfo resultInfo;
    std::vector<NativeAbiValueInfo> sourceArgInfos;
    llvm::FunctionType *llvmType = nullptr;

    const NativeAbiValueInfo &argInfo(std::size_t index) const {
        return sourceArgInfos.at(index);
    }

    NativeAbiPassKind argPassKind(std::size_t index) const {
        return argInfo(index).passKind;
    }
};

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
NativeAbiFunctionSignature classifyNativeFunctionAbi(TypeTable &types,
                                                     FuncType *funcType,
                                                     bool hasImplicitSelf = false);
llvm::FunctionType *getNativeAbiFunctionType(TypeTable &types, FuncType *funcType,
                                             bool hasImplicitSelf = false);

}  // namespace lona
