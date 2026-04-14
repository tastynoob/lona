#include "buildin.hh"

#include "parser.hh"

namespace lona {

namespace {

void
pinBuiltinType(TypeClass *type) {
    if (type) {
        type->retain();
    }
}

}  // namespace

void
IntType::binaryOperation(llvm::IRBuilder<>& builder, ObjectPtr left,
                         token_type op, ObjectPtr right, ObjectPtr& res) {
    // if (!left->getType()->equal(this)) throw "Type mismatch";
    // llvm::Value* val = nullptr;
    // switch (op) {
    //     case '+':
    //         val = builder.CreateAdd(left->get(builder), right->get(builder));
    //         break;
    //     case '-':
    //         val = builder.CreateSub(left->get(builder), right->get(builder));
    //         break;
    //     case '*':
    //         val = builder.CreateMul(left->get(builder), right->get(builder));
    //         break;
    //     case '/':
    //         if (isSigned()) {
    //             val =
    //                 builder.CreateSDiv(left->get(builder),
    //                 right->get(builder));
    //         }

    //         else {
    //             val =
    //                 builder.CreateUDiv(left->get(builder),
    //                 right->get(builder));
    //         }
    //         break;
    //     // boolean
    //     case '<':
    //         if (isSigned()) {
    //             val = builder.CreateICmpSLT(left->get(builder),
    //                                         right->get(builder));
    //         } else {
    //             val = builder.CreateICmpULT(left->get(builder),
    //                                         right->get(builder));
    //         }
    //         res = new BaseVar(val, boolTy, Object::REG_VAL |
    //         Object::READONLY); return;
    //     case '>':
    //         if (isSigned()) {
    //             val = builder.CreateICmpSGT(left->get(builder),
    //                                         right->get(builder));
    //         } else {
    //             val = builder.CreateICmpUGT(left->get(builder),
    //                                         right->get(builder));
    //         }
    //         res = new BaseVar(val, boolTy, Object::REG_VAL |
    //         Object::READONLY); return;
    //     case Parser::token_type::LOGIC_EQUAL:  // ==
    //         val = builder.CreateICmpEQ(left->get(builder),
    //         right->get(builder)); res = new BaseVar(val, boolTy,
    //         Object::REG_VAL | Object::READONLY); return;
    //     case Parser::token_type::LOGIC_NOT_EQUAL:  // !=
    //         val = builder.CreateICmpNE(left->get(builder),
    //         right->get(builder)); res = new BaseVar(val, boolTy,
    //         Object::REG_VAL | Object::READONLY); return;
    //     default:
    //         assert(false);
    //         break;
    // }
    // res = new BaseVar(val, left->getType(),
    //                    Object::REG_VAL | Object::READONLY);
}

void
IntType::unaryOperation(llvm::IRBuilder<>& builder, token_type op,
                        ObjectPtr value, ObjectPtr& res) {
    // if (!value->getType()->equal(this)) throw "Type mismatch";
    // ObjectPtr  val = nullptr;
    // switch (op) {
    //     case '+':
    //         val = value;
    //     case '-':
    //         val = new BaseVar(builder.CreateNeg(value->get(builder)), this,
    //                           Object::REG_VAL | Object::READONLY);
    //         break;
    //     case '!':
    //         val = new BaseVar(builder.CreateNot(value->get(builder)), boolTy,
    //                           Object::REG_VAL | Object::READONLY);
    //         break;
    //     case '~':
    //         val = new BaseVar(builder.CreateNot(value->get(builder)), this,
    //                           Object::REG_VAL | Object::READONLY);
    //         break;
    //     case '*':
    //         break;
    //     case '&':
    //         break;
    //     default:
    //         break;
    // }
    // res = val;
}

IntType* u8Ty = nullptr;
IntType* i8Ty = nullptr;
IntType* u16Ty = nullptr;
IntType* i16Ty = nullptr;
IntType* u32Ty = nullptr;
IntType* i32Ty = nullptr;
IntType* u64Ty = nullptr;
IntType* i64Ty = nullptr;
IntType* usizeTy = nullptr;
FLoatType* f32Ty = nullptr;
FLoatType* f64Ty = nullptr;
BoolType* boolTy = nullptr;

void
initBuildinType(Scope* scope) {
    auto* typeTable = scope ? scope->types() : nullptr;
    if (!typeTable) {
        return;
    }

    if (!u8Ty) {
        u8Ty = new IntType(BaseType::U8, "u8");
        i8Ty = new IntType(BaseType::I8, "i8");
        u16Ty = new IntType(BaseType::U16, "u16");
        i16Ty = new IntType(BaseType::I16, "i16");
        u32Ty = new IntType(BaseType::U32, "u32");
        i32Ty = new IntType(BaseType::I32, "i32");
        u64Ty = new IntType(BaseType::U64, "u64");
        i64Ty = new IntType(BaseType::I64, "i64");
        usizeTy = new IntType(BaseType::USIZE, "usize");
        f32Ty = new FLoatType(BaseType::F32, "f32");
        f64Ty = new FLoatType(BaseType::F64, "f64");
        boolTy = new BoolType();
        for (auto *type : {static_cast<TypeClass *>(u8Ty),
                           static_cast<TypeClass *>(i8Ty),
                           static_cast<TypeClass *>(u16Ty),
                           static_cast<TypeClass *>(i16Ty),
                           static_cast<TypeClass *>(u32Ty),
                           static_cast<TypeClass *>(i32Ty),
                           static_cast<TypeClass *>(u64Ty),
                           static_cast<TypeClass *>(i64Ty),
                           static_cast<TypeClass *>(usizeTy),
                           static_cast<TypeClass *>(f32Ty),
                           static_cast<TypeClass *>(f64Ty),
                           static_cast<TypeClass *>(boolTy)}) {
            pinBuiltinType(type);
        }
    }

    typeTable->addType(string("u8"), u8Ty);
    typeTable->addType(string("i8"), i8Ty);
    typeTable->addType(string("u16"), u16Ty);
    typeTable->addType(string("i16"), i16Ty);
    typeTable->addType(string("u32"), u32Ty);
    typeTable->addType(string("i32"), i32Ty);
    typeTable->addType(string("u64"), u64Ty);
    typeTable->addType(string("i64"), i64Ty);
    typeTable->addType(string("usize"), usizeTy);
    typeTable->addType(string("int"), i32Ty);
    typeTable->addType(string("uint"), u32Ty);
    typeTable->addType(string("f32"), f32Ty);
    typeTable->addType(string("f64"), f64Ty);
    typeTable->addType(string("bool"), boolTy);
}
}
