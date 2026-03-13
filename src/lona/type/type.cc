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
        case Type::I32:
            return llvm::Type::getInt32Ty(env.getContext());
        default:
            throw "Unsupported base type";  // TODO: more
    }
}

}  // namespace lona