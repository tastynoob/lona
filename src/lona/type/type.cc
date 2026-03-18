#include "type.hh"

namespace lona {

bool
TupleType::getMember(llvm::StringRef name, ValueTy &member) const {
    if (!name.consume_front("_") || name.empty()) {
        return false;
    }

    unsigned index = 0;
    if (name.getAsInteger(10, index) || index == 0 ||
        index > itemTypes.size()) {
        return false;
    }

    member = {itemTypes[index - 1], static_cast<int>(index - 1)};
    return true;
}

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

ObjectPtr
TupleType::newObj(uint32_t specifiers) {
    return new TupleVar(this, specifiers);
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
    for (std::size_t i = 0; i < argTypes.size(); ++i) {
        auto *argType = argTypes[i];
        if (getArgBindingKind(i) == BindingKind::Ref) {
            llvmArgTypes.push_back(
                types.getLLVMType(types.createPointerType(argType)));
            continue;
        }
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
    bool ok = false;
    auto extents = staticDimensions(&ok);
    if (!ok || extents.empty()) {
        return llvm::PointerType::getUnqual(types.getLLVMType(elementType));
    }

    llvm::Type *llvmType = types.getLLVMType(elementType);
    for (auto it = extents.rbegin(); it != extents.rend(); ++it) {
        llvmType = llvm::ArrayType::get(llvmType, static_cast<std::uint64_t>(*it));
    }
    return llvmType;
}

}  // namespace lona
