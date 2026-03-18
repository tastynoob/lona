#include "array_dim.hh"
#include "astnode.hh"
#include <string>

namespace lona {

bool
isLegacyArrayDimensionPrefix(const std::vector<AstNode *> &dimensions) {
    return !dimensions.empty() && dimensions.front() == nullptr;
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
    if (!constant || constant->getType() != AstConst::Type::INT32) {
        return false;
    }
    value = *constant->getBuf<std::int32_t>();
    return true;
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
