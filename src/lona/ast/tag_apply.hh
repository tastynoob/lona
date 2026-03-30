#pragma once

namespace lona {

class AstNode;

AstNode *
applyBuiltinTags(AstNode *node);
void
validateBuiltinTagResults(AstNode *node);

}  // namespace lona
