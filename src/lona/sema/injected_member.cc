#include "injected_member.hh"

#include "lona/ast/astnode.hh"
#include "lona/ast/token.hh"
#include "lona/sema/operator_resolver.hh"
#include "lona/type/buildin.hh"
#include <algorithm>

namespace lona {

bool
isInjectedMemberFloatFamily(TypeClass *type) {
    return isFloatType(type);
}

bool
isInjectedMemberIntegerFamily(TypeClass *type) {
    return isIntegerType(type);
}

unsigned
injectedMemberNumericBitWidth(TypeTable *typeTable, TypeClass *type) {
    auto *base = asUnqualified<BaseType>(type);
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
    case BaseType::USIZE: {
        if (typeTable) {
            const auto byteCount = typeTable->getTypeAllocSize(type);
            if (byteCount > 0) {
                return static_cast<unsigned>(byteCount * 8);
            }
        }
        if (type->typeSize > 0) {
            return static_cast<unsigned>(type->typeSize * 8);
        }
        return 0;
    }
    default:
        return 0;
    }
}

TypeClass *
injectedMemberSignedIntegerTypeForWidth(unsigned width) {
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

TypeClass *
injectedMemberUnsignedIntegerTypeForWidth(unsigned width) {
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

TypeClass *
injectedMemberFloatTypeForWidth(unsigned width) {
    switch (width) {
    case 32:
        return f32Ty;
    case 64:
        return f64Ty;
    default:
        return nullptr;
    }
}

AstNode *
makeInjectedMemberArrayDimension(std::int64_t value) {
    AstToken token(TokenType::ConstInt32, std::to_string(value).c_str(), location());
    return new AstConst(token);
}

bool
isInjectedMemberBitsArrayType(TypeClass *type,
                              std::int64_t *byteCount = nullptr) {
    auto *array = asUnqualified<ArrayType>(type);
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

ArrayType *
getOrCreateInjectedMemberBitsArrayType(TypeTable *typeTable,
                                       std::int64_t byteCount) {
    std::vector<AstNode *> dims;
    dims.push_back(makeInjectedMemberArrayDimension(byteCount));
    return typeTable->createArrayType(u8Ty, std::move(dims));
}

bool canImplicitNumericConversion(TypeClass *targetType, TypeClass *sourceType) {
    if (!targetType || !sourceType || targetType == sourceType) {
        return false;
    }
    if (isInjectedMemberIntegerFamily(targetType) &&
        isInjectedMemberIntegerFamily(sourceType)) {
        return true;
    }
    if (isInjectedMemberFloatFamily(targetType) &&
        isInjectedMemberFloatFamily(sourceType)) {
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
    const bool sourceBitsArray =
        isInjectedMemberBitsArrayType(sourceType);
    const bool targetBitsArray =
        isInjectedMemberBitsArrayType(targetType);
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

TypeClass *commonNumericType(TypeTable *typeTable, TypeClass *leftType,
                             TypeClass *rightType) {
    if (!leftType || !rightType || leftType == rightType) {
        return leftType;
    }

    if (isInjectedMemberFloatFamily(leftType) &&
        isInjectedMemberFloatFamily(rightType)) {
        return injectedMemberFloatTypeForWidth(std::max(
            injectedMemberNumericBitWidth(typeTable, leftType),
            injectedMemberNumericBitWidth(typeTable, rightType)));
    }

    if (isInjectedMemberIntegerFamily(leftType) &&
        isInjectedMemberIntegerFamily(rightType)) {
        const bool bothSigned =
            isSignedIntegerType(leftType) && isSignedIntegerType(rightType);
        const bool bothUnsigned =
            isUnsignedIntegerType(leftType) && isUnsignedIntegerType(rightType);
        const auto leftWidth =
            injectedMemberNumericBitWidth(typeTable, leftType);
        const auto rightWidth =
            injectedMemberNumericBitWidth(typeTable, rightType);
        const auto width = std::max(leftWidth, rightWidth);
        if (bothSigned) {
            return injectedMemberSignedIntegerTypeForWidth(width);
        }
        if (bothUnsigned) {
            return injectedMemberUnsignedIntegerTypeForWidth(width);
        }
        auto *signedType = isSignedIntegerType(leftType) ? leftType : rightType;
        auto *unsignedType = isUnsignedIntegerType(leftType) ? leftType : rightType;
        const auto signedWidth =
            injectedMemberNumericBitWidth(typeTable, signedType);
        const auto unsignedWidth =
            injectedMemberNumericBitWidth(typeTable, unsignedType);
        if (signedWidth > unsignedWidth) {
            return injectedMemberSignedIntegerTypeForWidth(signedWidth);
        }
        return injectedMemberUnsignedIntegerTypeForWidth(width);
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
        auto *arrayType = getOrCreateInjectedMemberBitsArrayType(
            typeTable, std::max<std::int64_t>(1, byteCount));
        return InjectedMemberBinding{InjectedMemberKind::BitCopy, "tobits",
                                     receiverType, arrayType};
    }
    const bool receiverIsBitsArray =
        isInjectedMemberBitsArrayType(receiverType);
    if (!receiverIsBitsArray) {
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

    return InjectedMemberBinding{InjectedMemberKind::BitCopy, memberName.str(),
                                 receiverType, targetType};
}

}  // namespace lona
