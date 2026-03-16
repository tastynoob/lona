#include "func.hh"
#include "lona/module/compilation_unit.hh"
#include "../type/scope.hh"
#include "../type/type.hh"
#include "../type/buildin.hh"
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

    if (this->getType() != src->getType()) {
        throw "type mismatch";
    }

    assert(val->getType()->isPointerTy());
    builder.CreateStore(src->get(scope), val);
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
    if (type == boolTy) {
        val = llvm::ConstantInt::get(scope->getLLVMType(type),
                                     std::any_cast<bool>(value));
        return val;
    }

    throw "unsupported const type";
}

Object *
StructVar::getField(Scope *scope, std::string name) {
    auto &builder = scope->builder;
    if (isRegVal()) {
        throw "struct field access on register value is not supported";
    }

    auto *structType = type->as<StructType>();
    assert(structType);
    auto *member = structType->getMember(
        llvm::StringRef(name.c_str(), name.size()));
    if (!member) {
        throw "unknown struct field";
    }

    auto *fieldType = member->first;
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
        llvm::ConstantInt::get(builder.getInt32Ty(), type->typeSize);
        builder.CreateMemCpy(val, llvm::MaybeAlign(8), struct_src->val,
                             llvm::MaybeAlign(8), type->typeSize);
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

Object *
emitFunctionCall(Scope *scope, llvm::Value *calleeValue, FuncType *funcType,
                 std::vector<Object *> &args) {
    auto &builder = scope->builder;
    auto *llvmFuncType = scope->getLLVMFunctionType(funcType);
    auto *retType = funcType->getRetType();
    const auto &argTypes = funcType->getArgTypes();
    std::vector<llvm::Value *> llvmargs;
    Object *retval = nullptr;

    if (retType && retType->shouldReturnByPointer()) {
        retval = retType->newObj(Object::VARIABLE);
        retval->createllvmValue(scope);
        llvmargs.push_back(retval->getllvmValue());
    }

    // check args type
    if (argTypes.size() == args.size())
        for (int i = 0; i < args.size(); i++) {
            if (args[i]->getType() != argTypes[i]) {
                throw "Call argument type mismatch";
            }
            llvmargs.push_back(args[i]->get(scope));
        }
    else {
        throw "Call argument number mismatch";
    }

    auto *ret = builder.CreateCall(llvmFuncType, calleeValue, llvmargs);

    if (retType && retType->shouldReturnByPointer()) {
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
