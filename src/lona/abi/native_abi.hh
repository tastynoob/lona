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

struct NativeAbiFunctionSignature {
    FuncType *sourceType = nullptr;
    bool hasImplicitSelf = false;
    bool hasIndirectResult = false;
    std::vector<NativeAbiPassKind> sourceArgPassKinds;
    llvm::FunctionType *llvmType = nullptr;

    NativeAbiPassKind argPassKind(std::size_t index) const {
        return sourceArgPassKinds.at(index);
    }
};

bool isNativeAbiAggregateType(TypeClass *type);
bool isNativeAbiDirectType(TypeClass *type);
bool usesNativeAbiIndirectResult(TypeClass *type);
NativeAbiFunctionSignature classifyNativeFunctionAbi(TypeTable &types,
                                                     FuncType *funcType,
                                                     bool hasImplicitSelf = false);
llvm::FunctionType *getNativeAbiFunctionType(TypeTable &types, FuncType *funcType,
                                             bool hasImplicitSelf = false);

}  // namespace lona
