#pragma once

#include "ast/astnode.hh"

namespace lona {

void
interpreter(AstNode *node);
void
compile(std::string &filepath, std::ostream &os);

}  // namespace lona