#include "type/type.hh"
#include "type.hh"
#include "type/buildin.hh"

namespace lona {

Object *
TypeClass::newObj(llvm::Value *val, uint32_t specifiers) {
    return new BaseVar(val, this, specifiers);
}

PointerType *
TypeClass::getPointerType(Scope *scope) {
    auto pointer_size =
        scope->module.getDataLayout().getPointerTypeSize(this->llvmType);
    return new PointerType(this, pointerType_pointer, pointer_size);
}

bool
PointerType::is(TypeClass *t) {
    if (auto right = dynamic_cast<PointerType *>(t)) {
        return originalType->is(right) && pointerLevels == right->pointerLevels;
    }
    return false;
}

Object *
StructType::newObj(llvm::Value *val, uint32_t specifiers) {
    return new StructVar(val, this, specifiers);
}

Object *
StructType::fieldSelect(llvm::IRBuilder<> &builder, Object *value,
                        const std::string &field) {
    if (members.find(field) == members.end()) {
        throw "Has no such member: " + field;
    }

    auto [membertype, index] = members[field];

    auto ret =
        builder.CreateStructGEP(this->llvmType, value->getllvmValue(), index);
    return membertype->newObj(ret);
}

Object *
FuncType::callOperation(Scope *scope, Object *value,
                        std::vector<Object *> args) {
    assert(dynamic_cast<Method *>(value));
    auto &builder = scope->builder;
    auto func = dynamic_cast<Method *>(value);
    std::vector<llvm::Value *> llvmargs;
    Object *retval = nullptr;

    auto start_arg_idx = 0;
    if (retType && retType->isPassByPointer()) {
        retval = scope->allocate(retType);
        llvmargs.push_back(retval->getllvmValue());
        start_arg_idx++;
    }

    // check args type
    if (argTypes.size() + start_arg_idx == args.size())
        for (int i = start_arg_idx; i < args.size(); i++) {
            if (args[i]->getType() != argTypes[i]) {
                throw "Call argument type mismatch";
            }
            llvmargs.push_back(args[i]->get(builder));
        }
    else {
        throw "Call argument number mismatch";
    }

    auto ret =
        builder.CreateCall((llvm::Function *)func->get(builder), llvmargs);

    if (retType && retType->isPassByPointer()) {
        return retval;
    } else if (retType) {
        return retType->newObj(ret, Object::REG_VAL);
    } else {
        // no return
        return nullptr;
    }
}

}  // namespace lona