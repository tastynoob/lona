#include "native_abi.hh"
#include <llvm-18/llvm/IR/DerivedTypes.h>

namespace lona {

namespace {

bool
isSingleRegisterSroaSize(std::uint64_t size) {
    switch (size) {
    case 1:
    case 2:
    case 4:
    case 8:
        return true;
    default:
        return false;
    }
}

llvm::IntegerType *
getPackedAggregateLLVMType(TypeTable &types, TypeClass *type) {
    if (!isNativeAbiAggregateType(type)) {
        return nullptr;
    }
    auto size = types.getTypeAllocSize(type);
    if (!isSingleRegisterSroaSize(size)) {
        return nullptr;
    }
    return llvm::IntegerType::get(types.getContext(),
                                  static_cast<unsigned>(size * 8));
}

llvm::Value *
coerceDirectValue(llvm::IRBuilder<> &builder, llvm::Value *value,
                  llvm::Type *targetType) {
    if (!value || !targetType || value->getType() == targetType) {
        return value;
    }
    if (value->getType()->isIntegerTy() && targetType->isIntegerTy()) {
        return builder.CreateZExtOrTrunc(value, targetType);
    }
    return builder.CreateBitCast(value, targetType);
}

llvm::Value *
bitcastPointerForLoadStore(llvm::IRBuilder<> &builder, llvm::Value *ptr,
                           llvm::Type *pointeeType) {
    auto *typedPtr = llvm::PointerType::getUnqual(pointeeType);
    return ptr->getType() == typedPtr ? ptr : builder.CreateBitCast(ptr, typedPtr);
}

}  // namespace

bool
isNativeAbiAggregateType(TypeClass *type) {
    return type && (type->as<StructType>() || type->as<TupleType>() ||
                    type->as<ArrayType>());
}

bool
usesNativeAbiPackedDirectType(TypeTable &types, TypeClass *type) {
    return isNativeAbiAggregateType(type) &&
        getPackedAggregateLLVMType(types, type) != nullptr;
}

bool
isNativeAbiDirectType(TypeTable &types, TypeClass *type) {
    return type && (type->as<BaseType>() || isPointerLikeType(type) ||
                    usesNativeAbiPackedDirectType(types, type));
}

bool
usesNativeAbiIndirectResult(TypeTable &types, TypeClass *type) {
    return isNativeAbiAggregateType(type) &&
        !usesNativeAbiPackedDirectType(types, type);
}

llvm::Type *
getNativeAbiDirectLLVMType(TypeTable &types, TypeClass *type) {
    if (!type) {
        return nullptr;
    }
    if (auto *packed = getPackedAggregateLLVMType(types, type)) {
        return packed;
    }
    return types.getLLVMType(type);
}

llvm::Value *
packNativeAbiDirectValue(llvm::IRBuilder<> &builder, TypeTable &types,
                         TypeClass *type, llvm::Value *value) {
    if (!type || !value) {
        return value;
    }
    auto *directType = getNativeAbiDirectLLVMType(types, type);
    if (!usesNativeAbiPackedDirectType(types, type)) {
        return coerceDirectValue(builder, value, directType);
    }

    auto *logicalType = types.getLLVMType(type);
    auto *temp = builder.CreateAlloca(logicalType);
    builder.CreateStore(coerceDirectValue(builder, value, logicalType), temp);
    return loadNativeAbiDirectValue(builder, types, type, temp);
}

llvm::Value *
loadNativeAbiDirectValue(llvm::IRBuilder<> &builder, TypeTable &types,
                         TypeClass *type, llvm::Value *sourcePtr) {
    if (!type || !sourcePtr) {
        return nullptr;
    }
    auto *directType = getNativeAbiDirectLLVMType(types, type);
    if (!usesNativeAbiPackedDirectType(types, type)) {
        return builder.CreateLoad(directType, sourcePtr);
    }

    auto *typedPtr = bitcastPointerForLoadStore(builder, sourcePtr, directType);
    return builder.CreateLoad(directType, typedPtr);
}

void
storeNativeAbiDirectValue(llvm::IRBuilder<> &builder, TypeTable &types,
                          TypeClass *type, llvm::Value *value,
                          llvm::Value *destPtr) {
    if (!type || !value || !destPtr) {
        return;
    }
    auto *directType = getNativeAbiDirectLLVMType(types, type);
    auto *coercedValue = coerceDirectValue(builder, value, directType);
    if (!usesNativeAbiPackedDirectType(types, type)) {
        builder.CreateStore(coercedValue, destPtr);
        return;
    }

    auto *typedPtr = bitcastPointerForLoadStore(builder, destPtr, directType);
    builder.CreateStore(coercedValue, typedPtr);
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
    signature.hasIndirectResult = usesNativeAbiIndirectResult(types, retType);
    signature.resultInfo.passKind = signature.hasIndirectResult
        ? NativeAbiPassKind::IndirectValue
        : NativeAbiPassKind::Direct;
    signature.resultInfo.llvmType = signature.hasIndirectResult
        ? nullptr
        : getNativeAbiDirectLLVMType(types, retType);
    signature.resultInfo.packedAggregate =
        retType && usesNativeAbiPackedDirectType(types, retType);

    const auto &argTypes = funcType->getArgTypes();
    signature.sourceArgInfos.reserve(argTypes.size());
    for (std::size_t i = 0; i < argTypes.size(); ++i) {
        NativeAbiValueInfo info;
        if ((hasImplicitSelf && i == 0) ||
            funcType->getArgBindingKind(i) == BindingKind::Ref) {
            info.passKind = NativeAbiPassKind::IndirectRef;
        } else if (isNativeAbiAggregateType(argTypes[i])) {
            if (usesNativeAbiPackedDirectType(types, argTypes[i])) {
                info.passKind = NativeAbiPassKind::Direct;
                info.packedAggregate = true;
                info.llvmType = getNativeAbiDirectLLVMType(types, argTypes[i]);
            } else {
                info.passKind = NativeAbiPassKind::IndirectValue;
            }
        } else {
            info.passKind = NativeAbiPassKind::Direct;
            info.llvmType = getNativeAbiDirectLLVMType(types, argTypes[i]);
        }
        if (!info.llvmType) {
            info.llvmType = info.passKind == NativeAbiPassKind::Direct
                ? getNativeAbiDirectLLVMType(types, argTypes[i])
                : types.getLLVMType(types.createPointerType(argTypes[i]));
        }
        signature.sourceArgInfos.push_back(info);
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
        llvmArgTypes.push_back(signature.sourceArgInfos[i].llvmType);
    }

    auto *llvmRetType = signature.hasIndirectResult || !retType
        ? llvm::Type::getVoidTy(types.getContext())
        : signature.resultInfo.llvmType;
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
