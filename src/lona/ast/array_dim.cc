#include "array_dim.hh"
#include "astnode.hh"
#include <limits>
#include <string>

namespace lona {

bool
isLegacyArrayDimensionPrefix(const std::vector<AstNode *> &dimensions) {
    return !dimensions.empty() && dimensions.front() == nullptr;
}

bool
isBareUnsizedArraySyntax(const std::vector<AstNode *> &dimensions) {
    return dimensions.empty();
}

std::size_t
arrayIndexArity(const std::vector<AstNode *> &dimensions) {
    std::size_t count = 0;
    for (auto *dimension : dimensions) {
        if (dimension != nullptr) {
            ++count;
        }
    }
    return count;
}

bool
hasUnsizedArrayDimensions(const std::vector<AstNode *> &dimensions) {
    if (dimensions.empty()) {
        return true;
    }
    for (auto *dimension : dimensions) {
        if (dimension == nullptr) {
            return true;
        }
    }
    return false;
}

bool
tryExtractArrayDimension(const AstNode *node, std::int64_t &value) {
    auto *constant = node ? dynamic_cast<const AstConst *>(node) : nullptr;
    if (!constant || !constant->isIntegerLiteral() ||
        constant->isUnaryMinusOnlySignedMinLiteral()) {
        return false;
    }
    switch (constant->getType()) {
    case AstConst::Type::I8:
        value = *constant->getBuf<std::int8_t>();
        return true;
    case AstConst::Type::U8:
        value = *constant->getBuf<std::uint8_t>();
        return true;
    case AstConst::Type::I16:
        value = *constant->getBuf<std::int16_t>();
        return true;
    case AstConst::Type::U16:
        value = *constant->getBuf<std::uint16_t>();
        return true;
    case AstConst::Type::I32:
        value = *constant->getBuf<std::int32_t>();
        return true;
    case AstConst::Type::U32:
        value = *constant->getBuf<std::uint32_t>();
        return true;
    case AstConst::Type::I64:
        value = *constant->getBuf<std::int64_t>();
        return true;
    case AstConst::Type::U64: {
        const auto raw = *constant->getBuf<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        value = static_cast<std::int64_t>(raw);
        return true;
    }
    case AstConst::Type::USIZE: {
        const auto raw = *constant->getBuf<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        value = static_cast<std::int64_t>(raw);
        return true;
    }
    default:
        return false;
    }
}

std::string
describeArrayDimensions(const std::vector<AstNode *> &dimensions) {
    std::string text = "[";
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
        if (i != 0) {
            text += ",";
        }
        if (dimensions[i] == nullptr) {
            continue;
        }

        std::int64_t value = 0;
        if (tryExtractArrayDimension(dimensions[i], value)) {
            text += std::to_string(value);
        } else {
            text += "?";
        }
    }
    text += "]";
    return text;
}

}  // namespace lona
