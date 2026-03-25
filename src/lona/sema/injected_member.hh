#pragma once

#include "lona/type/type.hh"
#include <optional>
#include <string>

namespace lona {

enum class InjectedMemberKind {
    BitCopy,
};

struct InjectedMemberBinding {
    InjectedMemberKind kind = InjectedMemberKind::BitCopy;
    std::string name;
    TypeClass *receiverType = nullptr;
    TypeClass *resultType = nullptr;
};

bool canImplicitNumericConversion(TypeClass *targetType, TypeClass *sourceType);
bool canExplicitNumericConversion(TypeClass *targetType, TypeClass *sourceType);
TypeClass *commonNumericType(TypeTable *typeTable, TypeClass *leftType,
                             TypeClass *rightType);
bool canExplicitBitCopy(TypeClass *targetType, TypeClass *sourceType);
std::optional<InjectedMemberBinding>
resolveInjectedMember(TypeTable *typeTable, TypeClass *receiverType,
                      llvm::StringRef memberName);

}  // namespace lona
