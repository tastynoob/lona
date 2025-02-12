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
        case Parser::token_type::LOGIC_EQUAL:
            val =
                builder.CreateICmpEQ(left->read(builder), right->read(builder));
            return new RValue(val, boolTy, Object::READ_NO_LOAD);
        default:
            break;
    }
    return new RValue(val, left->getType(), Object::READ_NO_LOAD);
}

Object*
I32Type::unaryOperation(llvm::IRBuilder<>& builder, token_type op,
                        Object* value) {
    if (!value->getType()->is(this)) throw "Type mismatch";
    Object* val = nullptr;
    switch (op) {
        case '!':

            break;
        case '~':

            break;
        case '*':

            break;
        case '&':
            if (!value->isVariable()) throw "Can't take address of const value";
            val = new RValue(value->getllvmValue(),
                             new PointerType(value->getType()),
                             Object::READ_NO_LOAD);
            break;
        default:
            break;
    }
    return val;
}

Object*
I32Type::assignOperation(llvm::IRBuilder<>& builder, Object* dst, Object* src) {
    if (!dst->getType()->is(this)) throw "Type mismatch";
    builder.CreateStore(src->read(builder), dst->getllvmValue());
    return nullptr;
}

I32Type* i32Ty = nullptr;
BoolType* boolTy = nullptr;

void
initBuildinType(Scope* scope) {
    if (!i32Ty) i32Ty = new I32Type(scope->getBuilder().getInt32Ty());
    if (!boolTy) boolTy = new BoolType(scope->getBuilder().getInt1Ty());
    scope->addType("i32", i32Ty);
    scope->addType("bool", boolTy);
}

}
