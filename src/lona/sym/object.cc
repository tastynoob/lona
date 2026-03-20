#include "func.hh"
#include "lona/module/compilation_unit.hh"
#include "../type/scope.hh"
#include "../type/type.hh"
#include "../type/buildin.hh"
#include <cassert>

namespace lona {
namespace {

llvm::Value *
reinterpretValueBits(Scope *scope, llvm::Value *value, TypeClass *srcType,
                     TypeClass *dstType) {
    if (!scope || !value || !srcType || !dstType || srcType == dstType) {
        return value;
    }
    if (!isByteCopyCompatible(dstType, srcType)) {
        throw "type mismatch";
    }

    auto *srcLLVMType = scope->getLLVMType(srcType);
    auto *dstLLVMType = scope->getLLVMType(dstType);
    if (srcLLVMType == dstLLVMType && value->getType() == dstLLVMType) {
        return value;
    }

    auto sourceBitWidth = static_cast<unsigned>(scope->types()->getTypeAllocSize(srcType) * 8);
    auto *bitsType = llvm::IntegerType::get(scope->builder.getContext(),
                                            sourceBitWidth);
    llvm::Value *bits = value;
    if (value->getType()->isPointerTy()) {
        bits = scope->builder.CreatePtrToInt(value, bitsType);
    } else if (value->getType()->isFloatingPointTy()) {
        bits = scope->builder.CreateBitCast(value, bitsType);
    } else if (value->getType()->isIntegerTy()) {
        if (value->getType() != bitsType) {
            bits = scope->builder.CreateZExtOrTrunc(value, bitsType);
        }
    } else {
        throw "type mismatch";
    }

    if (dstLLVMType->isPointerTy()) {
        return scope->builder.CreateIntToPtr(bits, dstLLVMType);
    }
    if (dstLLVMType->isFloatingPointTy()) {
        return scope->builder.CreateBitCast(bits, dstLLVMType);
    }
    if (dstLLVMType->isIntegerTy()) {
        if (bits->getType() == dstLLVMType) {
            return bits;
        }
        return scope->builder.CreateZExtOrTrunc(bits, dstLLVMType);
    }
    throw "type mismatch";
}

llvm::Value *
coerceObjectValue(Scope *scope, Object *src, TypeClass *dstType) {
    if (!scope || !src || !dstType) {
        return nullptr;
    }
    auto *srcType = src->getType();
    auto *value = src->get(scope);
    return reinterpretValueBits(scope, value, srcType, dstType);
}

}  // namespace


void
Object::createllvmValue(Scope *scope)
{
    assert(!val && !isRegVal());
    if (isVariable()) {
        val = scope->allocate(type);
    }
}

llvm::Value *
Object::get(Scope *scope) {
    auto &builder = scope->builder;
    if (isRegVal()) {
        if (val) {
            return val;
        }
        if (auto *constant = dynamic_cast<ConstVar *>(this)) {
            return constant->ConstVar::get(scope);
        }
        throw "register value is not materialized";
    }
    assert(val->getType()->isPointerTy());
    return builder.CreateLoad(scope->getLLVMType(type), val);
}

void
Object::set(Scope *scope, Object *src) {
    auto &builder = scope->builder;
    if (isReadOnly() || isRegVal()) {
        throw "readonly";
    }

    if (!isByteCopyCompatible(this->getType(), src->getType())) {
        throw "type mismatch";
    }

    assert(val->getType()->isPointerTy());
    builder.CreateStore(coerceObjectValue(scope, src, this->getType()), val);
}

llvm::Value *
ConstVar::get(Scope *scope) {
    auto &builder = scope->builder;
    if (val) {
        return val;
    }

    if (type == i32Ty) {
        val = llvm::ConstantInt::get(scope->getLLVMType(type),
                                     std::any_cast<int32_t>(value), true);
        return val;
    }
    if (type == f32Ty) {
        val = llvm::ConstantFP::get(scope->getLLVMType(type),
                                    std::any_cast<float>(value));
        return val;
    }
    if (type == f64Ty) {
        val = llvm::ConstantFP::get(scope->getLLVMType(type),
                                    std::any_cast<double>(value));
        return val;
    }
    if (type == boolTy) {
        val = llvm::ConstantInt::get(scope->getLLVMType(type),
                                     std::any_cast<bool>(value));
        return val;
    }

    throw "unsupported const type";
}

Object *
TupleVar::getField(Scope *scope, std::string name) {
    auto &builder = scope->builder;
    auto *tupleType = type->as<TupleType>();
    assert(tupleType);

    TupleType::ValueTy member;
    if (!tupleType->getMember(llvm::StringRef(name.c_str(), name.size()), member)) {
        throw "unknown tuple field";
    }

    auto *fieldType = member.first;
    auto fieldIndex = static_cast<unsigned>(member.second);
    if (isRegVal()) {
        auto *aggregate = get(scope);
        auto *field = fieldType->newObj(Object::REG_VAL | Object::READONLY);
        field->bindllvmValue(
            builder.CreateExtractValue(aggregate, {fieldIndex}));
        return field;
    }

    auto *field = fieldType->newObj(Object::VARIABLE);
    field->setllvmValue(builder.CreateStructGEP(scope->getLLVMType(type), val,
                                                fieldIndex));
    return field;
}

Object *
StructVar::getField(Scope *scope, std::string name) {
    auto &builder = scope->builder;
    auto *structType = type->as<StructType>();
    assert(structType);
    auto *member = structType->getMember(
        llvm::StringRef(name.c_str(), name.size()));
    if (!member) {
        throw "unknown struct field";
    }

    auto *fieldType = member->first;
    if (isRegVal()) {
        auto *aggregate = get(scope);
        auto *field = fieldType->newObj(Object::REG_VAL | Object::READONLY);
        field->bindllvmValue(
            builder.CreateExtractValue(aggregate, {static_cast<unsigned>(member->second)}));
        return field;
    }

    auto *field = fieldType->newObj(Object::VARIABLE);
    field->setllvmValue(builder.CreateStructGEP(scope->getLLVMType(type), val,
                                                member->second));
    return field;
}

void
StructVar::set(Scope *scope, Object *src) {
    auto &builder = scope->builder;
    if (this->getType() != src->getType()) {
        throw "type mismatch";
    }

    if (src->isRegVal()) {
        builder.CreateStore(src->get(scope), val);
    } else {
        auto struct_src = dynamic_cast<StructVar *>(src);
        auto typeSize = scope->types()->getTypeAllocSize(type);
        llvm::ConstantInt::get(builder.getInt32Ty(), typeSize);
        builder.CreateMemCpy(val, llvm::MaybeAlign(8), struct_src->val,
                             llvm::MaybeAlign(8), typeSize);
    }
}

llvm::Value *
ModuleObject::get(Scope *scope) {
    throw "module namespace is not a runtime value";
}

void
ModuleObject::set(Scope *scope, Object *src) {
    throw "module namespace is read-only";
}

llvm::Value *
TypeObject::get(Scope *scope) {
    throw "type name is not a runtime value";
}

void
TypeObject::set(Scope *scope, Object *src) {
    throw "type name is read-only";
}

Object *
emitFunctionCall(Scope *scope, llvm::Value *calleeValue, FuncType *funcType,
                 std::vector<Object *> &args) {
    auto &builder = scope->builder;
    auto *llvmFuncType = scope->getLLVMFunctionType(funcType);
    auto *retType = funcType->getRetType();
    const auto &argTypes = funcType->getArgTypes();
    std::vector<llvm::Value *> llvmargs;
    Object *retval = nullptr;

    if (retType && scope->types()->shouldReturnByPointer(retType)) {
        retval = retType->newObj(Object::VARIABLE);
        retval->createllvmValue(scope);
        llvmargs.push_back(retval->getllvmValue());
    }

    // check args type
    if (argTypes.size() == args.size())
        for (int i = 0; i < args.size(); i++) {
            if (funcType->getArgBindingKind(i) == BindingKind::Ref) {
                if (argTypes[i] != args[i]->getType()) {
                    throw "Call reference argument type mismatch";
                }
                if (!args[i]->isVariable() || args[i]->isRegVal() ||
                    !args[i]->getllvmValue()) {
                    throw "Call reference argument must be addressable";
                }
                llvmargs.push_back(args[i]->getllvmValue());
                continue;
            }
            if (!isByteCopyCompatible(argTypes[i], args[i]->getType())) {
                throw "Call argument type mismatch";
            }
            llvmargs.push_back(coerceObjectValue(scope, args[i], argTypes[i]));
        }
    else {
        throw "Call argument number mismatch";
    }

    auto *ret = builder.CreateCall(llvmFuncType, calleeValue, llvmargs);

    if (retType && scope->types()->shouldReturnByPointer(retType)) {
        return retval;
    } else if (retType) {
        auto obj = retType->newObj(Object::REG_VAL);
        obj->bindllvmValue(ret);
        return obj;
    } else {
        // no return
        return nullptr;
    }

    return nullptr;
}

Object *
Function::call(Scope *scope, std::vector<Object *> &args) {
    auto *funcType = type->as<FuncType>();
    return emitFunctionCall(scope, val, funcType, args);
}

}  // namespace lona
