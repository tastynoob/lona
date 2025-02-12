#include "type/value.hh"
#include "type/typeclass.hh"
#include "value.hh"

namespace lona {

llvm::Value *
Object::read(llvm::IRBuilder<> &builder) {
    if (isReadNoLoad()) {
        return val;
    }
    assert(val->getType()->isPointerTy());
    return builder.CreateLoad(type->getllvmType(), val);
}

void
Object::write(llvm::IRBuilder<> &builder, Object *src) {
    sizeof(std::shared_ptr<Object>);
    if (this->getType() != src->getType()) {
        throw "type mismatch";
    }
    assert(val->getType()->isPointerTy());
    type->assignOperation(builder, this, src);
}

void
StructVar::write(llvm::IRBuilder<> &builder, Object *src) {
    if (!dynamic_cast<StructVar *>(src)) {
        throw "struct use memcpy";
    }
    auto struct_src = dynamic_cast<StructVar *>(src);
    llvm::DataLayout dataLayout(builder.GetInsertBlock()->getModule());
    auto struct_size = dataLayout.getTypeSizeInBits(type->getllvmType()) / 8;
    // struct use memcpy
    llvm::ConstantInt::get(builder.getInt32Ty(), struct_size);
    builder.CreateMemCpy(val, llvm::MaybeAlign(8), struct_src->val,
                         llvm::MaybeAlign(8), struct_size);
}

}  // namespace lona
