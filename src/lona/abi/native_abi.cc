#include "native_abi.hh"
#include <llvm-18/llvm/IR/DerivedTypes.h>
#include <string>

namespace lona {

namespace {

constexpr std::uint64_t kNativeAbiDirectAggregateReturnMaxSize = 16;

bool
isSingleRegisterPackSize(std::uint64_t size) {
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

bool
hasNativeAbiFixedAggregateLayout(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    if (!storageType) {
        return false;
    }
    if (storageType->as<StructType>() || storageType->as<TupleType>()) {
        return true;
    }
    if (auto *array = storageType->as<ArrayType>()) {
        return array->hasStaticLayout();
    }
    return false;
}

bool
usesNativeAbiDirectAggregateReturn(TypeTable &types, TypeClass *type) {
    // This keeps the aggregate result shape in the function signature.
    if (!isNativeAbiAggregateType(type) ||
        !hasNativeAbiFixedAggregateLayout(type)) {
        return false;
    }
    auto size = types.getTypeAllocSize(type);
    return size > 0 && size <= kNativeAbiDirectAggregateReturnMaxSize;
}

llvm::IntegerType *
getPackedRegisterAggregateLLVMType(TypeTable &types, TypeClass *type) {
    // This is ABI-level aggregate packing, not LLVM's local SROA pass.
    if (!isNativeAbiAggregateType(type) ||
        !hasNativeAbiFixedAggregateLayout(type)) {
        return nullptr;
    }
    auto size = types.getTypeAllocSize(type);
    if (!isSingleRegisterPackSize(size)) {
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

std::string
lonaNativeAbiVersionString() {
    return "v" + std::to_string(kLonaNativeAbiMajorVersion) + "." +
        std::to_string(kLonaNativeAbiMinorVersion);
}

std::string
lonaNativeAbiVersionSymbolName() {
    return "__lona_native_abi_v" + std::to_string(kLonaNativeAbiMajorVersion) +
        "_" + std::to_string(kLonaNativeAbiMinorVersion);
}

std::string
lonaNativeAbiVersionPayload() {
    return "lona.native_abi=" + lonaNativeAbiVersionString();
}

bool
isNativeAbiAggregateType(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    return storageType &&
        (storageType->as<StructType>() || storageType->as<TupleType>() ||
         storageType->as<ArrayType>());
}

bool
usesNativeAbiPackedRegisterAggregate(TypeTable &types, TypeClass *type) {
    return isNativeAbiAggregateType(type) &&
        getPackedRegisterAggregateLLVMType(types, type) != nullptr;
}

bool
usesNativeAbiIndirectResult(TypeTable &types, TypeClass *type) {
    return isNativeAbiAggregateType(type) &&
        !usesNativeAbiDirectAggregateReturn(types, type);
}

llvm::Type *
getNativeAbiDirectLLVMType(TypeTable &types, TypeClass *type) {
    if (!type) {
        return nullptr;
    }
    if (auto *packed = getPackedRegisterAggregateLLVMType(types, type)) {
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
    if (!usesNativeAbiPackedRegisterAggregate(types, type)) {
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
    if (!usesNativeAbiPackedRegisterAggregate(types, type)) {
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
    if (!usesNativeAbiPackedRegisterAggregate(types, type)) {
        builder.CreateStore(coercedValue, destPtr);
        return;
    }

    auto *typedPtr = bitcastPointerForLoadStore(builder, destPtr, directType);
    builder.CreateStore(coercedValue, typedPtr);
}

AbiFunctionSignature
classifyNativeFunctionAbi(TypeTable &types, FuncType *funcType,
                          bool hasImplicitSelf) {
    AbiFunctionSignature signature;
    signature.sourceType = funcType;
    signature.abiKind = AbiKind::Native;
    signature.hasImplicitSelf = hasImplicitSelf;
    if (!funcType) {
        return signature;
    }

    auto *retType = funcType->getRetType();
    signature.hasIndirectResult = usesNativeAbiIndirectResult(types, retType);
    signature.hasDirectAggregateResult =
        retType && !signature.hasIndirectResult &&
        isNativeAbiAggregateType(retType) &&
        !usesNativeAbiPackedRegisterAggregate(types, retType);
    signature.resultInfo.passKind = signature.hasIndirectResult
        ? AbiPassKind::IndirectValue
        : AbiPassKind::Direct;
    signature.resultInfo.llvmType = signature.hasIndirectResult
        ? nullptr
        : getNativeAbiDirectLLVMType(types, retType);
    signature.resultInfo.packedRegisterAggregate =
        retType && usesNativeAbiPackedRegisterAggregate(types, retType);

    const auto &argTypes = funcType->getArgTypes();
    signature.sourceArgInfos.reserve(argTypes.size());
    for (std::size_t i = 0; i < argTypes.size(); ++i) {
        AbiValueInfo info;
        if (funcType->getArgBindingKind(i) == BindingKind::Ref) {
            info.passKind = AbiPassKind::IndirectRef;
        } else if (isNativeAbiAggregateType(argTypes[i])) {
            if (usesNativeAbiPackedRegisterAggregate(types, argTypes[i])) {
                info.passKind = AbiPassKind::Direct;
                info.packedRegisterAggregate = true;
                info.llvmType = getNativeAbiDirectLLVMType(types, argTypes[i]);
            } else {
                info.passKind = AbiPassKind::IndirectValue;
            }
        } else {
            info.passKind = AbiPassKind::Direct;
            info.llvmType = getNativeAbiDirectLLVMType(types, argTypes[i]);
        }
        if (!info.llvmType) {
            info.llvmType = info.passKind == AbiPassKind::Direct
                ? getNativeAbiDirectLLVMType(types, argTypes[i])
                : types.getLLVMType(types.createPointerType(argTypes[i]));
        }
        signature.sourceArgInfos.push_back(info);
    }

    std::vector<llvm::Type *> llvmArgTypes;
    llvmArgTypes.reserve(argTypes.size() + (signature.hasIndirectResult ? 1 : 0));

    std::size_t startIndex = 0;
    if (hasImplicitSelf && !argTypes.empty()) {
        llvmArgTypes.push_back(signature.sourceArgInfos.front().llvmType);
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
