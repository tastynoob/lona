#include "type/typeclass.hh"
#include "typeclass.hh"
#include "type/buildin.hh"

namespace lona {

bool
lona::PointerType::is(TypeClass *t) {
    if (auto right = dynamic_cast<PointerType *>(t)) {
        return originalType->is(right) && pointerLevels == right->pointerLevels;
    }
    return false;
}

TypeManger::TypeManger(llvm::IRBuilder<> &builder) : builder(builder) {
    typeMap.insert({"i32", new I32Type(builder)});
}

TypeClass *
TypeManger::getTypeClass(std::string *const full_typename) {
    auto it = typeMap.find(*full_typename);
    if (it != typeMap.end()) {
        return it->second;
    }
    return nullptr;
}
TypeClass *
TypeManger::getTypeClass(std::string const full_typename) {
    auto it = typeMap.find(full_typename);
    if (it != typeMap.end()) {
        return it->second;
    }
    return nullptr;
}



TypeClass *
TypeManger::getTypeClass(TypeHelper *const type) {
    std::string *final_type = nullptr;
    for (auto &it : type->typeName) {
        final_type = &it;
    }

    TypeClass *original_type = nullptr;
    if (final_type->front() == '!') {
        // function
    } else {
        // normal type
        original_type = getTypeClass(final_type);
    }
    return original_type;
}

}  // namespace lona