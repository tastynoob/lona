#pragma once

#include "astnode.hh"
#include <string>
#include <string_view>

namespace lona {

std::string
describeTypeNode(const TypeNode *node,
                 std::string_view nullDescription = "<unknown type>");

}  // namespace lona
