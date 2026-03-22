#include "native_abi.hh"
#include <llvm-18/llvm/IR/DerivedTypes.h>

namespace lona {

bool
isNativeAbiAggregateType(TypeClass *type) {
    return type && (type->as<StructType>() || type->as<TupleType>() ||
                    type->as<ArrayType>());
}

bool
isNativeAbiDirectType(TypeClass *type) {
    return type && (type->as<BaseType>() || isPointerLikeType(type));
}

bool
usesNativeAbiIndirectResult(TypeClass *type) {
    return isNativeAbiAggregateType(type);
}

NativeAbiFunctionSignature
classifyNativeFunctionAbi(TypeTable &types, FuncType *funcType,
                          bool hasImplicitSelf) {
    NativeAbiFunctionSignature signature;
    signature.sourceType = funcType;
    signature.hasImplicitSelf = hasImplicitSelf;
    if (!funcType) {
        return signature;
    }

    auto *retType = funcType->getRetType();
    signature.hasIndirectResult = usesNativeAbiIndirectResult(retType);

    const auto &argTypes = funcType->getArgTypes();
    signature.sourceArgPassKinds.reserve(argTypes.size());
    for (std::size_t i = 0; i < argTypes.size(); ++i) {
        NativeAbiPassKind kind = NativeAbiPassKind::Direct;
        if ((hasImplicitSelf && i == 0) ||
            funcType->getArgBindingKind(i) == BindingKind::Ref) {
            kind = NativeAbiPassKind::IndirectRef;
        } else if (isNativeAbiAggregateType(argTypes[i])) {
            kind = NativeAbiPassKind::IndirectValue;
        }
        signature.sourceArgPassKinds.push_back(kind);
    }

    std::vector<llvm::Type *> llvmArgTypes;
    llvmArgTypes.reserve(argTypes.size() + (signature.hasIndirectResult ? 1 : 0));

    std::size_t startIndex = 0;
    if (hasImplicitSelf && !argTypes.empty()) {
        llvmArgTypes.push_back(
            types.getLLVMType(types.createPointerType(argTypes.front())));
        startIndex = 1;
    }

    if (signature.hasIndirectResult) {
        llvmArgTypes.push_back(
            types.getLLVMType(types.createPointerType(retType)));
    }

    for (std::size_t i = startIndex; i < argTypes.size(); ++i) {
        auto kind = signature.sourceArgPassKinds[i];
        if (kind == NativeAbiPassKind::Direct) {
            llvmArgTypes.push_back(types.getLLVMType(argTypes[i]));
        } else {
            llvmArgTypes.push_back(
                types.getLLVMType(types.createPointerType(argTypes[i])));
        }
    }

    auto *llvmRetType = signature.hasIndirectResult || !retType
        ? llvm::Type::getVoidTy(types.getContext())
        : types.getLLVMType(retType);
    signature.llvmType =
        llvm::FunctionType::get(llvmRetType, llvmArgTypes, false);
    return signature;
}

llvm::FunctionType *
getNativeAbiFunctionType(TypeTable &types, FuncType *funcType,
                         bool hasImplicitSelf) {
    return classifyNativeFunctionAbi(types, funcType, hasImplicitSelf).llvmType;
}

}  // namespace lona
