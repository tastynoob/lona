#include "scope.hh"
#include <llvm-18/llvm/IR/BasicBlock.h>

namespace lona {
Scope::~Scope() {
    for (auto &it : variables) {
        delete it.second;
    }
}

llvm::Type *
Scope::getLLVMType(TypeClass *type) const {
    assert(typeTable);
    return typeTable->getLLVMType(type);
}

llvm::FunctionType *
Scope::getLLVMFunctionType(FuncType *type) const {
    assert(typeTable);
    return typeTable->getLLVMFunctionType(type);
}

void
Scope::bindMethodFunction(StructType *parent, llvm::StringRef name, Function *func) {
    assert(typeTable);
    typeTable->bindMethodFunction(parent, name, func);
}

Function *
Scope::getMethodFunction(const StructType *parent, llvm::StringRef name) const {
    assert(typeTable);
    return typeTable->getMethodFunction(parent, name);
}

void
Scope::addObj(llvm::StringRef name, Object *var) {
    if (variables.find(name) != variables.end()) {
        throw "variable already exists";
    }
    assert(var);
    variables[name] = var;
}

bool
Scope::hasLocalObj(llvm::StringRef name) const {
    return variables.find(name) != variables.end();
}

Object *
Scope::getObj(llvm::StringRef name) {
    if (variables.find(name) == variables.end()) {
        if (parent) {
            return parent->getObj(name);
        }
        return nullptr;
    }
    return variables[name];
}

llvm::Value *
GlobalScope::allocate(TypeClass *type, bool is_extern) {
    return nullptr;
}

llvm::Value *
FuncScope::allocate(TypeClass *type, bool is_temp) {
    auto &entryBB = builder.GetInsertBlock()->getParent()->getEntryBlock();
    auto *llvmType = getLLVMType(type);
    if (alloc_point) {
        if (alloc_point == &entryBB.back()) {
            alloc_point = new llvm::AllocaInst(llvmType, 0, "", &entryBB);
        } else {
            auto *next = alloc_point->getNextNode();
            if (next) {
                alloc_point = new llvm::AllocaInst(llvmType, 0, "", next);
            } else {
                alloc_point = new llvm::AllocaInst(llvmType, 0, "", &entryBB);
            }
        }
    } else {
        if (entryBB.empty()) {
            alloc_point = builder.CreateAlloca(llvmType);
        } else {
            auto t =
                new llvm::AllocaInst(llvmType, 0, "", &entryBB.front());
            alloc_point = t;
        }
    }

    return alloc_point;
}

}
