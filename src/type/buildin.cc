#include "buildin.hh"

#include "type/variable.hh"

namespace lona {

BaseVariable*
I32Type::binaryOperation(llvm::IRBuilder<> &builder, BaseVariable* left, token_type op, BaseVariable* right) {
    assert(left->getType()->is(this));
    llvm::Value* val = nullptr;
    switch (op) {
        case '+':
            val = builder.CreateAdd(left->read(builder), right->read(builder));
            break;
        case '-':
            val = builder.CreateSub(left->read(builder), right->read(builder));
            break;
        case '*':
            val = builder.CreateMul(left->read(builder), right->read(builder));
            break;
        case '/':
            val = builder.CreateSDiv(left->read(builder), right->read(builder));
            break;
        default:
            break;
    }
    return new BaseVariable(val, left->getType(), BaseVariable::CONST);
}

}

