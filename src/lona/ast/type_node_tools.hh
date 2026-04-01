#pragma once

#include "astnode.hh"
#include <llvm/ADT/StringRef.h>
#include <string>

namespace lona {

std::string
baseTypeName(const BaseTypeNode *node);
bool
splitBaseTypeName(const BaseTypeNode *node, std::string &moduleName,
                  std::string &memberName);
bool
isReservedInitialListTypeName(llvm::StringRef name);
bool
isReservedInitialListTypeNode(TypeNode *node);
[[noreturn]] void
errorReservedInitialListType(const location &loc);
[[noreturn]] void
errorPointerOnlyAnyType(const location &loc, const TypeNode *node);
TypeNode *
typeNodeFromBracketItem(AstNode *node);
TypeNode *
createBracketSuffixTypeNode(TypeNode *base, std::vector<AstNode *> *items,
                            const location &loc = location());
void
validateTypeNodeLayout(const TypeNode *node);

}  // namespace lona
