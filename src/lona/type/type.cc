#include "type.hh"
#include "../type/buildin.hh"
#include <llvm-18/llvm/IR/Type.h>

namespace lona {

ObjectPtr
TypeClass::newObj(Env& env, uint32_t specifiers) {
    return new BaseVar(this, specifiers);
}

llvm::Type*
BaseType::genLLVMType(Env& env) {
    switch (type) {
        case Type::U8:
        case Type::I8:
            return llvm::Type::getInt8Ty(env.getContext());
        case Type::U16:
        case Type::I16:
            return llvm::Type::getInt16Ty(env.getContext());
        case Type::U32:
        case Type::I32:
            return llvm::Type::getInt32Ty(env.getContext());
        case Type::U64:
        case Type::I64:
            return llvm::Type::getInt64Ty(env.getContext());
        case Type::F32:
            return llvm::Type::getFloatTy(env.getContext());
        case Type::F64:
            return llvm::Type::getDoubleTy(env.getContext());
        case Type::BOOL:
            return llvm::Type::getInt1Ty(env.getContext());
        default:
            throw "Unsupported base type";
    }
}

}  // namespace lona