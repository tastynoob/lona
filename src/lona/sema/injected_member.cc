#include "injected_member.hh"

#include "lona/ast/astnode.hh"
#include "lona/ast/token.hh"
#include "lona/sema/operator_resolver.hh"
#include "lona/type/buildin.hh"
#include <algorithm>

namespace lona {
namespace {

bool isFloatFamily(TypeClass *type) {
    return isFloatType(type);
}

bool isIntegerFamily(TypeClass *type) {
    return isIntegerType(type);
}

unsigned numericBitWidth(TypeClass *type) {
    auto *base = type ? type->as<BaseType>() : nullptr;
    if (!base) {
        return 0;
    }
    switch (base->type) {
    case BaseType::U8:
    case BaseType::I8:
        return 8;
    case BaseType::U16:
    case BaseType::I16:
        return 16;
    case BaseType::U32:
    case BaseType::I32:
    case BaseType::F32:
        return 32;
    case BaseType::U64:
    case BaseType::I64:
    case BaseType::F64:
        return 64;
    default:
        return 0;
    }
}

TypeClass *signedIntegerTypeForWidth(unsigned width) {
    switch (width) {
    case 8:
        return i8Ty;
    case 16:
        return i16Ty;
    case 32:
        return i32Ty;
    case 64:
        return i64Ty;
    default:
        return nullptr;
    }
}

TypeClass *unsignedIntegerTypeForWidth(unsigned width) {
    switch (width) {
    case 8:
        return u8Ty;
    case 16:
        return u16Ty;
    case 32:
        return u32Ty;
    case 64:
        return u64Ty;
    default:
        return nullptr;
    }
}

TypeClass *floatTypeForWidth(unsigned width) {
    switch (width) {
    case 32:
        return f32Ty;
    case 64:
        return f64Ty;
    default:
        return nullptr;
    }
}

AstNode *makeArrayDimension(std::int64_t value) {
    AstToken token(TokenType::ConstInt32, std::to_string(value).c_str(), location());
    return new AstConst(token);
}

bool isBitsArrayType(TypeClass *type, std::int64_t *byteCount = nullptr) {
    auto *array = type ? type->as<ArrayType>() : nullptr;
    if (!array || array->getElementType() != u8Ty || !array->hasStaticLayout()) {
        return false;
    }
    const auto dims = array->staticDimensions();
    if (dims.size() != 1) {
        return false;
    }
    if (byteCount) {
        *byteCount = dims[0];
    }
    return true;
}

ArrayType *getOrCreateBitsArrayType(TypeTable *typeTable, std::int64_t byteCount) {
    std::vector<AstNode *> dims;
    dims.push_back(makeArrayDimension(byteCount));
    return typeTable->createArrayType(u8Ty, std::move(dims));
}

}  // namespace

bool canImplicitNumericConversion(TypeClass *targetType, TypeClass *sourceType) {
    if (!targetType || !sourceType || targetType == sourceType) {
        return false;
    }
    if (isIntegerFamily(targetType) && isIntegerFamily(sourceType)) {
        return true;
    }
    if (isFloatFamily(targetType) && isFloatFamily(sourceType)) {
        return true;
    }
    return false;
}

bool canExplicitNumericConversion(TypeClass *targetType, TypeClass *sourceType) {
    if (!targetType || !sourceType || targetType == sourceType) {
        return false;
    }
    return isNumericType(targetType) && isNumericType(sourceType);
}

bool canExplicitBitCopy(TypeClass *targetType, TypeClass *sourceType) {
    if (!targetType || !sourceType || targetType == sourceType) {
        return false;
    }
    const bool sourceBitsArray = isBitsArrayType(sourceType);
    const bool targetBitsArray = isBitsArrayType(targetType);
    if ((!sourceBitsArray && !isByteCopyPlainType(sourceType)) ||
        (!targetBitsArray && !isByteCopyPlainType(targetType))) {
        return false;
    }
    if ((!sourceBitsArray && isBoolStorageType(sourceType)) ||
        (!targetBitsArray && isBoolStorageType(targetType))) {
        return false;
    }
    return sourceBitsArray || targetBitsArray;
}

TypeClass *commonNumericType(TypeClass *leftType, TypeClass *rightType) {
    if (!leftType || !rightType || leftType == rightType) {
        return leftType;
    }

    if (isFloatFamily(leftType) && isFloatFamily(rightType)) {
        return floatTypeForWidth(std::max(numericBitWidth(leftType),
                                          numericBitWidth(rightType)));
    }

    if (isIntegerFamily(leftType) && isIntegerFamily(rightType)) {
        const bool bothSigned =
            isSignedIntegerType(leftType) && isSignedIntegerType(rightType);
        const bool bothUnsigned =
            isUnsignedIntegerType(leftType) && isUnsignedIntegerType(rightType);
        const auto leftWidth = numericBitWidth(leftType);
        const auto rightWidth = numericBitWidth(rightType);
        const auto width = std::max(leftWidth, rightWidth);
        if (bothSigned) {
            return signedIntegerTypeForWidth(width);
        }
        if (bothUnsigned) {
            return unsignedIntegerTypeForWidth(width);
        }
        auto *signedType = isSignedIntegerType(leftType) ? leftType : rightType;
        auto *unsignedType = isUnsignedIntegerType(leftType) ? leftType : rightType;
        const auto signedWidth = numericBitWidth(signedType);
        const auto unsignedWidth = numericBitWidth(unsignedType);
        if (signedWidth > unsignedWidth) {
            return signedIntegerTypeForWidth(signedWidth);
        }
        return unsignedIntegerTypeForWidth(width);
    }

    return nullptr;
}

std::optional<InjectedMemberBinding>
resolveInjectedMember(TypeTable *typeTable, TypeClass *receiverType,
                      llvm::StringRef memberName) {
    if (!typeTable || !receiverType) {
        return std::nullopt;
    }
    if (memberName == "tobits") {
        if (!isNumericType(receiverType)) {
            return std::nullopt;
        }
        auto byteCount = static_cast<std::int64_t>(typeTable->getTypeAllocSize(receiverType));
        auto *arrayType = getOrCreateBitsArrayType(typeTable, std::max<std::int64_t>(1, byteCount));
        return InjectedMemberBinding{InjectedMemberKind::BitCopy, "tobits",
                                     receiverType, arrayType};
    }
    const bool receiverIsNumeric = isNumericType(receiverType);
    const bool receiverIsBitsArray = isBitsArrayType(receiverType);
    if (!receiverIsNumeric && !receiverIsBitsArray) {
        return std::nullopt;
    }
    if (!memberName.consume_front("to")) {
        return std::nullopt;
    }
    if (memberName.empty()) {
        return std::nullopt;
    }

    auto *targetType = typeTable->getType(memberName);
    if (!targetType || !isNumericType(targetType)) {
        return std::nullopt;
    }

    if (receiverIsNumeric) {
        return InjectedMemberBinding{InjectedMemberKind::NumericConversion,
                                     memberName.str(), receiverType, targetType};
    }
    return InjectedMemberBinding{InjectedMemberKind::BitCopy, memberName.str(),
                                 receiverType, targetType};
}

}  // namespace lona
