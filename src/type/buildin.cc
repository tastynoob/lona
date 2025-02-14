#include "buildin.hh"

#include "parser.hh"
#include "type/value.hh"

namespace lona {

Object*
I32Type::binaryOperation(llvm::IRBuilder<>& builder, Object* left,
                         token_type op, Object* right) {
    if (!left->getType()->is(this)) throw "Type mismatch";
    llvm::Value* val = nullptr;
    switch (op) {
        case '+':
            val = builder.CreateAdd(left->get(builder), right->get(builder));
            break;
        case '-':
            val = builder.CreateSub(left->get(builder), right->get(builder));
            break;
        case '*':
            val = builder.CreateMul(left->get(builder), right->get(builder));
            break;
        case '/':
            val = builder.CreateSDiv(left->get(builder), right->get(builder));
            break;
        case Parser::token_type::LOGIC_EQUAL:
            val = builder.CreateICmpEQ(left->get(builder), right->get(builder));
            return new BaseVar(val, boolTy, Object::REG_VAL | Object::READONLY);
        default:
            break;
    }
    return new BaseVar(val, left->getType(),
                       Object::REG_VAL | Object::READONLY);
}

Object*
I32Type::unaryOperation(llvm::IRBuilder<>& builder, token_type op,
                        Object* value) {
    if (!value->getType()->is(this)) throw "Type mismatch";
    Object* val = nullptr;
    switch (op) {
        case '+':
            val = value;
        case '-':
            val = new BaseVar(builder.CreateNeg(value->get(builder)), this,
                              Object::REG_VAL | Object::READONLY);
            break;
        case '!':
            val = new BaseVar(builder.CreateNot(value->get(builder)), boolTy,
                              Object::REG_VAL | Object::READONLY);
            break;
        case '~':
            val = new BaseVar(builder.CreateNot(value->get(builder)), this,
                              Object::REG_VAL | Object::READONLY);
            break;
        case '*':
            break;
        case '&':
            break;
        default:
            break;
    }
    return val;
}

Object*
I32Type::assignOperation(llvm::IRBuilder<>& builder, Object* dst, Object* src) {
    if (!dst->getType()->is(this)) throw "Type mismatch";
    builder.CreateStore(src->get(builder), dst->getllvmValue());
    return nullptr;
}

I32Type* i32Ty = nullptr;
BoolType* boolTy = nullptr;

void
initBuildinType(Scope* scope) {
    if (!i32Ty) i32Ty = new I32Type(scope->builder.getInt32Ty());
    if (!boolTy) boolTy = new BoolType(scope->builder.getInt1Ty());
    scope->addType("i32", i32Ty);
    scope->addType("bool", boolTy);
}

}
