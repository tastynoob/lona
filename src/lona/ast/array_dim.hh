#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lona {

class AstNode;

bool
isLegacyArrayDimensionPrefix(const std::vector<AstNode *> &dimensions);
bool
isBareUnsizedArraySyntax(const std::vector<AstNode *> &dimensions);
std::size_t
arrayIndexArity(const std::vector<AstNode *> &dimensions);
bool
hasUnsizedArrayDimensions(const std::vector<AstNode *> &dimensions);
std::string
describeArrayDimensions(const std::vector<AstNode *> &dimensions);
bool
tryExtractArrayDimension(const AstNode *node, std::int64_t &value);

}  // namespace lona
