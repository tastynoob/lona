#include "value.hh"
#include "../type/scope.hh"
#include "../type/type.hh"
#include <cassert>

namespace lona {


void
Object::createllvmValue(Scope *scope)
{
    assert(!val && !isRegVal());
    if (isVariable()) {
        val = scope->allocate(type);
    }
}

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
    if (isReadOnly() || isRegVal()) {
        throw "readonly";
    }

    if (this->getType() != src->getType()) {
        throw "type mismatch";
    }

    assert(val->getType()->isPointerTy());
    builder.CreateStore(src->get(builder), val);
}

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

Object *
Function::call(Scope *scope, std::vector<Object *> &args) {
    auto &builder = scope->builder;
    auto llvm_func = (llvm::Function *)val;
    auto retType = type->as<FuncType>()->getRetType();
    auto argTypes = type->as<FuncType>()->getArgTypes();
    std::vector<llvm::Value *> llvmargs;
    Object *retval = nullptr;

    if (retType && retType->shouldReturnByPointer()) {
        assert(false);
    }

    // check args type
    if (argTypes.size() == args.size())
        for (int i = 0; i < args.size(); i++) {
            if (args[i]->getType() != argTypes[i]) {
                throw "Call argument type mismatch";
            }
            llvmargs.push_back(args[i]->get(builder));
        }
    else {
        throw "Call argument number mismatch";
    }

    auto ret = builder.CreateCall(llvm_func, llvmargs);

    if (retType && retType->shouldReturnByPointer()) {
        return retval;
    } else if (retType) {
        auto obj = retType->newObj(Object::REG_VAL);
        //obj->bindllvmValue(ret);
        return obj;
    } else {
        // no return
        return nullptr;
    }

    return nullptr;
}

}  // namespace lona
