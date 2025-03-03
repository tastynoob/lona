#include "buildin.hh"

#include "obj/value.hh"
#include "parser.hh"

namespace lona {

Object*
IntType::binaryOperation(llvm::IRBuilder<>& builder, Object* left,
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
            if (isSigned()) {
                val =
                    builder.CreateSDiv(left->get(builder), right->get(builder));
            }

            else {
                val =
                    builder.CreateUDiv(left->get(builder), right->get(builder));
            }
            break;
        // boolean
        case '<':
            if (isSigned()) {
                val = builder.CreateICmpSLT(left->get(builder),
                                            right->get(builder));
            } else {
                val = builder.CreateICmpULT(left->get(builder),
                                            right->get(builder));
            }
            return new BaseVar(val, boolTy, Object::REG_VAL | Object::READONLY);
        case '>':
            if (isSigned()) {
                val = builder.CreateICmpSGT(left->get(builder),
                                            right->get(builder));
            } else {
                val = builder.CreateICmpUGT(left->get(builder),
                                            right->get(builder));
            }
            return new BaseVar(val, boolTy, Object::REG_VAL | Object::READONLY);
        case Parser::token_type::LOGIC_EQUAL:  // ==
            val = builder.CreateICmpEQ(left->get(builder), right->get(builder));
            return new BaseVar(val, boolTy, Object::REG_VAL | Object::READONLY);
        case Parser::token_type::LOGIC_NOT_EQUAL:  // !=
            val = builder.CreateICmpNE(left->get(builder), right->get(builder));
            return new BaseVar(val, boolTy, Object::REG_VAL | Object::READONLY);
        default:
            assert(false);
            break;
    }
    return new BaseVar(val, left->getType(),
                       Object::REG_VAL | Object::READONLY);
}

Object*
IntType::unaryOperation(llvm::IRBuilder<>& builder, token_type op,
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

IntType* i8Ty = nullptr;
IntType* i16Ty = nullptr;
IntType* i32Ty = nullptr;
IntType* i64Ty = nullptr;
FLoatType* f32Ty = nullptr;
FLoatType* f64Ty = nullptr;
BoolType* boolTy = nullptr;

void
initBuildinType(Scope* scope) {

    i8Ty = new IntType(scope->builder.getInt8Ty(), BaseType::I8, "i8");
    i16Ty = new IntType(scope->builder.getInt16Ty(), BaseType::I16, "i16");
    i32Ty = new IntType(scope->builder.getInt32Ty(), BaseType::I32, "i32");
    i64Ty = new IntType(scope->builder.getInt64Ty(), BaseType::I64, "i64");
    f32Ty = new FLoatType(scope->builder.getFloatTy(), BaseType::F32, "f32");
    f64Ty = new FLoatType(scope->builder.getDoubleTy(), BaseType::F64, "f64");
    boolTy = new BoolType(scope->builder.getInt1Ty());

    scope->addType("i8", i8Ty);
    scope->addType("i16", i16Ty);
    scope->addType("i32", i32Ty);
    scope->addType("i64", i64Ty);
    scope->addType("bool", boolTy);
}
}
