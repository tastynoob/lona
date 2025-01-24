#include "type/typeclass.hh"
#include "typeclass.hh"

namespace lona {

bool
lona::PointerType::is(TypeClass *t) {
    if (auto right = dynamic_cast<PointerType *>(t)) {
        return originalType->is(right) && pointerLevels == right->pointerLevels;
    }
    return false;
}

TypeManger::TypeManger(llvm::IRBuilder<> &builder) : builder(builder) {
    typeMap.insert({"i8", new BaseType(builder.getInt8Ty(), BaseType::I8)});
    typeMap.insert({"u8", new BaseType(builder.getInt16Ty(), BaseType::U8)});
    typeMap.insert({"i16", new BaseType(builder.getInt16Ty(), BaseType::I16)});
    typeMap.insert({"u16", new BaseType(builder.getInt16Ty(), BaseType::U16)});
    typeMap.insert({"i32", new BaseType(builder.getInt32Ty(), BaseType::I32)});
    typeMap.insert({"u32", new BaseType(builder.getInt32Ty(), BaseType::U32)});
    typeMap.insert({"i64", new BaseType(builder.getInt64Ty(), BaseType::I64)});
    typeMap.insert({"u64", new BaseType(builder.getInt64Ty(), BaseType::U64)});
    typeMap.insert({"f32", new BaseType(builder.getFloatTy(), BaseType::F32)});
    typeMap.insert({"f64", new BaseType(builder.getDoubleTy(), BaseType::F64)});
    typeMap.insert({"bool", new BaseType(builder.getInt1Ty(), BaseType::BOOL)});
    typeMap.insert({"str", new PointerType(typeMap[std::string("i8")])});
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