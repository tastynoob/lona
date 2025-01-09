#pragma once

#include "ast/astnode.hh"

namespace lona {

void
interpreter(AstNode *node);
void
compile(AstNode *node, std::string &filename, std::ostream &os);

}  // namespace lona