#include "type.hh"
#include "../type/buildin.hh"

namespace lona {

ObjectPtr
TypeClass::newObj(uint32_t specifiers) {
    return new BaseVar(this, specifiers);
}

ObjectPtr
StructType::newObj(uint32_t specifiers) {
    return new StructVar(this, specifiers);
}

void
StructType::fieldSelect(llvm::IRBuilder<> &builder, ObjectPtr value,
                        const std::string &field, ObjectPtr& res) {
    if (members.find(field) == members.end()) {
        throw "Has no such member: " + field;
    }

    auto [membertype, index] = members[field];

    auto ret =
        builder.CreateStructGEP(this->llvmType, value->getllvmValue(), index);
    res = membertype->newObj();
}

void
FuncType::callOperation(Scope *scope, ObjectPtr value,
                        std::vector<ObjectPtr> args, ObjectPtr& res) {
    assert(dynamic_cast<Function *>(value));
    auto &builder = scope->builder;
    auto func = dynamic_cast<Function *>(value);
    std::vector<llvm::Value *> llvmargs;

    if (retType && hasSROA) {
        if (res == nullptr) {
            res = scope->allocate(retType, true);
        }
        llvmargs.push_back(res->getllvmValue());
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

    auto ret =
        builder.CreateCall((llvm::Function *)func->get(builder), llvmargs);

    if (retType && hasSROA) {
        // nothing
    } else if (retType) {
        res = retType->newObj(Object::REG_VAL);
    } else {
        // no return
        res = nullptr;
    }
}

TypeClass *
TypeManager::getType(TypeNode* typeNode)
{




}

}  // namespace lona