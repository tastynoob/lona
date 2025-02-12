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

void
Scope::addType(llvm::StringRef name, TypeClass *type) {
    bool exists = false;
    try {
        if (getType(name)) {
            exists = true;
        }
    } catch (std::string &e) {
        // nothing
    }
    if (exists) {
        throw "type already exists";
    }
    typeMap[name] = type;
}

TypeClass *
Scope::getType(llvm::StringRef name) {
    if (typeMap.find(name) == typeMap.end()) {
        if (parent) {
            return parent->getType(name);
        }
        throw("undefined type: " + name).str();
    }
    return typeMap[name];
}

TypeClass *
Scope::getType(TypeHelper *const type) {
    std::string *final_type = nullptr;
    for (auto &it : type->typeName) {
        final_type = &it;
    }

    TypeClass *original_type = nullptr;
    if (final_type->front() == '!') {
        // function
    } else {
        // normal type
        original_type = getType(*final_type);
    }
    if (original_type == nullptr) throw "unknown type";
    return original_type;
}

Object *
GlobalScope::allocate(TypeClass *type, bool is_extern) {
    return nullptr;
}

Object *
FuncScope::allocate(TypeClass *type, bool is_temp) {
    auto &entryBB = builder.GetInsertBlock()->getParent()->getEntryBlock();
    if (!alloc_point) {
        if (entryBB.empty()) {
            alloc_point = builder.CreateAlloca(type->getllvmType());
        } else {
            auto t = new llvm::AllocaInst(type->getllvmType(), 0, "",
                                          entryBB.front().getNextNode());
            alloc_point = t;
        }
    } else {
        if (alloc_point == &entryBB.back()) {
            alloc_point = builder.CreateAlloca(type->getllvmType());
        } else {
            auto t = new llvm::AllocaInst(type->getllvmType(), 0, "",
                                          (alloc_point)->getNextNode());
            alloc_point = t;
        }
    }

    return type->newObj(alloc_point);
}

}
