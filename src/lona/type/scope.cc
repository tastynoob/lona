#include "scope.hh"

namespace lona {
Scope::~Scope() {
    for (auto &it : variables) {
        delete it.second;
    }
}

void
Scope::addObj(llvm::StringRef name, Object *var) {
    if (variables.find(name) != variables.end()) {
        throw "variable already exists";
    }
    assert(var);
    variables[name] = var;
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

Object *
GlobalScope::allocate(TypeClass *type, bool is_extern) {
    return nullptr;
}

Object *
FuncScope::allocate(TypeClass *type, bool is_temp) {
    auto &entryBB = builder.GetInsertBlock()->getParent()->getEntryBlock();
    assert(type->llvmType);
    if (alloc_point) {
        if (alloc_point == &entryBB.back()) {
            alloc_point = builder.CreateAlloca(type->llvmType);
        } else {
            auto t = new llvm::AllocaInst(type->llvmType, 0, "",
                                          (alloc_point)->getNextNode());
            alloc_point = t;
        }
    } else {
        if (entryBB.empty()) {
            alloc_point = builder.CreateAlloca(type->llvmType);
        } else {
            volatile int i = 0;
            auto t =
                new llvm::AllocaInst(type->llvmType, 0, "", &entryBB.front());
            alloc_point = t;
        }
    }

    auto obj = type->newObj();
    obj->bindllvmValue(alloc_point);
    return obj;
}

}
