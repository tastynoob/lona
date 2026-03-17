#include "type.hh"

namespace lona {

llvm::Type *
BaseType::buildLLVMType(TypeTable& types) {
    switch (type) {
        case Type::U8:
        case Type::I8:
            return llvm::Type::getInt8Ty(types.getContext());
        case Type::U16:
        case Type::I16:
            return llvm::Type::getInt16Ty(types.getContext());
        case Type::U32:
        case Type::I32:
            return llvm::Type::getInt32Ty(types.getContext());
        case Type::U64:
        case Type::I64:
            return llvm::Type::getInt64Ty(types.getContext());
        case Type::F32:
            return llvm::Type::getFloatTy(types.getContext());
        case Type::F64:
            return llvm::Type::getDoubleTy(types.getContext());
        case Type::BOOL:
            return llvm::Type::getInt1Ty(types.getContext());
        default:
            throw "Unsupported base type";
    }
}

llvm::Type *
StructType::buildLLVMType(TypeTable &types) {
    return llvm::StructType::create(types.getContext(),
                                    full_name.tochara());
}

llvm::Type *
TupleType::buildLLVMType(TypeTable &types) {
    std::vector<llvm::Type *> memberTypes;
    memberTypes.reserve(itemTypes.size());
    for (auto *itemType : itemTypes) {
        memberTypes.push_back(types.getLLVMType(itemType));
    }
    return llvm::StructType::get(types.getContext(), memberTypes, false);
}

llvm::Type *
FuncType::buildLLVMType(TypeTable &types) {
    std::vector<llvm::Type *> llvmArgTypes;
    llvmArgTypes.reserve(argTypes.size() + (hasSROA ? 1 : 0));
    llvm::Type *llvmRetType = nullptr;
    if (hasSROA) {
        llvmArgTypes.push_back(
            types.getLLVMType(types.createPointerType(retType)));
        llvmRetType = llvm::Type::getVoidTy(types.getContext());
    } else {
        llvmRetType = retType
            ? types.getLLVMType(retType)
            : llvm::Type::getVoidTy(types.getContext());
    }
    for (auto *argType : argTypes) {
        llvmArgTypes.push_back(types.getLLVMType(argType));
    }
    return llvm::FunctionType::get(llvmRetType, llvmArgTypes, false);
}

llvm::Type *
PointerType::buildLLVMType(TypeTable &types) {
    return llvm::PointerType::getUnqual(types.getLLVMType(pointeeType));
}

llvm::Type *
ArrayType::buildLLVMType(TypeTable &types) {
    return llvm::PointerType::getUnqual(types.getLLVMType(elementType));
}

}  // namespace lona
