#pragma once

#include "lona/sema/hir.hh"

namespace lona {

class ModuleInterface;
class TypeTable;
class AstNode;

std::string
describeResolvedType(TypeClass *type);

std::string
numericConversionHint();
std::string
bitCopyHint();
std::string
nullLiteralHint();
std::string
pointerConversionHint();

bool
isNullLiteralExpr(HIRExpr *expr);
bool
isRawMemoryPointerType(TypeClass *type);
bool
isIndexablePointerType(TypeClass *type);
bool
canImplicitPointerViewConversion(TypeClass *targetType, TypeClass *sourceType);
bool
canExplicitPointerRebindCast(TypeClass *targetType, TypeClass *sourceType);

bool
isSupportedStaticLiteralInitializerExpr(AstNode *node);
TypeClass *
inferStaticLiteralInitializerType(ModuleInterface *interface, AstNode *init);

void
requireCompatibleInitializerTypes(const location &loc, TypeClass *expectedType,
                                  TypeClass *actualType,
                                  const std::string &context);

HIRExpr *
coerceNumericInitializerExpr(TypeTable *typeMgr, HIRModule *ownerModule,
                             HIRExpr *expr, TypeClass *targetType,
                             const location &loc, bool explicitRequest);

HIRExpr *
coercePointerInitializerExpr(TypeTable *typeMgr, HIRModule *ownerModule,
                             HIRExpr *expr, TypeClass *targetType,
                             const location &loc, bool explicitCast = false);

HIRExpr *
analyzeStaticLiteralInitializerExpr(TypeTable *typeMgr, HIRModule *ownerModule,
                                    AstNode *node,
                                    TypeClass *expectedType = nullptr);

}  // namespace lona
