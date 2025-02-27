#include "type/type.hh"
#include "value.hh"

namespace lona {

llvm::Value *
Object::get(llvm::IRBuilder<> &builder) {
    if (isRegVal()) {
        return val;
    }
    assert(val->getType()->isPointerTy());
    return builder.CreateLoad(type->llvmType, val);
}

void
Object::set(llvm::IRBuilder<> &builder, Object *src) {
    if (isReadOnly()) {
        throw "readonly";
    }

    if (this->getType() != src->getType()) {
        throw "type mismatch";
    }

    assert(val->getType()->isPointerTy());
    builder.CreateStore(src->get(builder), val);
}

// llvm::Value *
// StructVar::get(llvm::IRBuilder<> &builder) {
//     return Object::get(builder);
// }

void
StructVar::set(llvm::IRBuilder<> &builder, Object *src) {
    if (this->getType() != src->getType()) {
        throw "type mismatch";
    }

    if (src->isRegVal()) {
        builder.CreateStore(src->get(builder), val);
    } else {
        auto struct_src = dynamic_cast<StructVar *>(src);
        llvm::ConstantInt::get(builder.getInt32Ty(), type->typeSize);
        builder.CreateMemCpy(val, llvm::MaybeAlign(8), struct_src->val,
                             llvm::MaybeAlign(8), type->typeSize);
    }
}

}  // namespace lona
