
#include "type/variable.hh"
#include "type/typeclass.hh"

namespace lona {

llvm::Value *
BaseVariable::read(llvm::IRBuilder<> &builder) {
    if (isVariable()) {
        assert(val->getType()->isPointerTy());
        return builder.CreateLoad(type->getllvmType(), val);
    }
    return val;
}

void
BaseVariable::write(llvm::IRBuilder<> &builder, BaseVariable *src) {
    if (isConst()) {
        throw "try to write const value";
    }
    if (this->getType() != src->getType()) {
        throw "type mismatch";
    }
    builder.CreateStore(src->val, val);
}

BaseVariable *
VariableManger::getVariable(llvm::StringRef name) {
    assert(!variables.empty());
    for (auto scope = variables.rbegin(); scope != variables.rend(); scope++) {
        auto it = scope->find(name);
        if (it != scope->end()) {
            return it->second;
        }
    }
}

}  // namespace lona
