#include "c_abi.hh"
#include <stdexcept>

namespace lona {

namespace {

std::string
toStdString(const string &value) {
    return std::string(value.tochara(), value.size());
}

bool
isCAbiV0DirectType(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    return storageType &&
        (storageType->as<BaseType>() || storageType->as<PointerType>() ||
         storageType->as<IndexablePointerType>());
}

llvm::Type *
getCAbiDirectLLVMType(TypeTable &types, TypeClass *type) {
    if (!type) {
        return nullptr;
    }
    if (!isCAbiV0DirectType(type)) {
        throw std::runtime_error("unsupported C ABI v0 type `" +
                                 toStdString(type->full_name) + "`");
    }
    return types.getLLVMType(type);
}

}  // namespace

AbiFunctionSignature
classifyCFunctionAbi(TypeTable &types, FuncType *funcType, bool hasImplicitSelf) {
    AbiFunctionSignature signature;
    signature.sourceType = funcType;
    signature.abiKind = AbiKind::C;
    signature.hasImplicitSelf = hasImplicitSelf;
    if (!funcType) {
        return signature;
    }
    if (hasImplicitSelf) {
        throw std::runtime_error("extern \"C\" methods are not supported");
    }

    auto *retType = funcType->getRetType();
    signature.resultInfo.passKind = AbiPassKind::Direct;
    signature.resultInfo.llvmType = getCAbiDirectLLVMType(types, retType);

    const auto &argTypes = funcType->getArgTypes();
    signature.sourceArgInfos.reserve(argTypes.size());
    for (std::size_t i = 0; i < argTypes.size(); ++i) {
        if (funcType->getArgBindingKind(i) == BindingKind::Ref) {
            throw std::runtime_error("extern \"C\" ref parameters are not supported");
        }
        AbiValueInfo info;
        info.passKind = AbiPassKind::Direct;
        info.llvmType = getCAbiDirectLLVMType(types, argTypes[i]);
        signature.sourceArgInfos.push_back(info);
    }

    std::vector<llvm::Type *> llvmArgTypes;
    llvmArgTypes.reserve(argTypes.size());
    for (const auto &info : signature.sourceArgInfos) {
        llvmArgTypes.push_back(info.llvmType);
    }

    auto *llvmRetType = retType ? signature.resultInfo.llvmType
                                : llvm::Type::getVoidTy(types.getContext());
    signature.llvmType =
        llvm::FunctionType::get(llvmRetType, llvmArgTypes, false);
    return signature;
}

llvm::FunctionType *
getCAbiFunctionType(TypeTable &types, FuncType *funcType, bool hasImplicitSelf) {
    return classifyCFunctionAbi(types, funcType, hasImplicitSelf).llvmType;
}

}  // namespace lona
