#pragma once

#include "../type/type.hh"
#include <cstddef>
#include <vector>

namespace lona {

enum class AbiPassKind {
    Direct,
    IndirectValue,
    IndirectRef,
};

struct AbiValueInfo {
    AbiPassKind passKind = AbiPassKind::Direct;
    llvm::Type *llvmType = nullptr;
    bool packedRegisterAggregate = false;
};

struct AbiFunctionSignature {
    FuncType *sourceType = nullptr;
    AbiKind abiKind = AbiKind::Native;
    bool hasImplicitSelf = false;
    bool hasIndirectResult = false;
    bool hasDirectAggregateResult = false;
    AbiValueInfo resultInfo;
    std::vector<AbiValueInfo> sourceArgInfos;
    llvm::FunctionType *llvmType = nullptr;

    const AbiValueInfo &argInfo(std::size_t index) const {
        return sourceArgInfos.at(index);
    }

    AbiPassKind argPassKind(std::size_t index) const {
        return argInfo(index).passKind;
    }
};

AbiFunctionSignature classifyFunctionAbi(TypeTable &types, FuncType *funcType,
                                         bool hasImplicitSelf = false);
llvm::FunctionType *getFunctionAbiLLVMType(TypeTable &types, FuncType *funcType,
                                           bool hasImplicitSelf = false);

}  // namespace lona
