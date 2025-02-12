#include "type/typeclass.hh"
#include "type/buildin.hh"
#include "typeclass.hh"

namespace lona {

Object *
TypeClass::newObj(llvm::Value *val) {
    return new Variable(val, this); 
}

bool
PointerType::is(TypeClass *t) {
    if (auto right = dynamic_cast<PointerType *>(t)) {
        return originalType->is(right) && pointerLevels == right->pointerLevels;
    }
    return false;
}

Object *
StructType::newObj(llvm::Value *val) {
    return new StructVar(val, this);
}

Object *
StructType::fieldSelect(llvm::IRBuilder<> &builder, Object *value,
                        const std::string &field) {

    if (members.find(field) == members.end()) {
        throw "Has no such member: " + field;
    }
    auto [membertype, index] = members[field];

    auto ret = builder.CreateStructGEP(this->getllvmType(),
                                       value->read(builder), index);
    return new Variable(ret, membertype);
}

Object *
FuncType::callOperation(llvm::IRBuilder<> &builder, Object *value,
                        std::vector<Object *> args) {
    assert(dynamic_cast<Functional *>(value));
    auto func = dynamic_cast<Functional *>(value);
    std::vector<llvm::Value *> llvmargs;
    // check args type
    if (argTypes.size() == args.size())
        for (int i = 0; i < args.size(); i++) {
            if (args[i]->getType() != argTypes[i]) {
                throw "Call argument type mismatch";
            }
            llvmargs.push_back(args[i]->read(builder));
        }
    else {
        throw "Call argument number mismatch";
    }

    auto ret =
        builder.CreateCall((llvm::Function *)func->read(builder), llvmargs);

    return new Variable(ret, retType);
}

}  // namespace lona