#pragma once

#include "lona/sema/hir.hh"
#include "lona/sema/injectedmember.hh"
#include <optional>
#include <string>

namespace lona {
namespace analysis_impl {

std::string
describeResolvedFuncType(FuncType *type, size_t argOffset = 0);

std::string
describeStorageType(TypeClass *type, AstVarDef *node);

std::string
describeMemberOwnerSyntax(const AstNode *node);

std::string
describeTupleFieldHelp(TupleType *tupleType);

std::string
castDomainHint();

bool
isBuiltinCastType(TypeClass *type);

bool
isDirectFunctionPointerValueType(TypeClass *type);

bool
isFixedArrayOfFunctionPointerValues(TypeClass *type);

std::string
describeInjectedMemberHelp(TypeClass *receiverType,
                           const std::string &memberName);

FuncType *
getMethodSelectorType(TypeTable *typeMgr, HIRSelector *selector);

size_t
getMethodCallArgOffset(HIRSelector *selector, FuncType *type);

std::string
describeMethodSelectorType(HIRSelector *selector, FuncType *type);

void
rejectMethodSelectorStorage(TypeTable *typeMgr, HIRExpr *expr, AstVarDef *node);

void
rejectNonCallMethodSelector(TypeTable *typeMgr, HIRExpr *expr);

std::optional<InjectedMemberBinding>
resolveInjectedMemberBinding(TypeTable *typeMgr, TypeClass *receiverType,
                             const std::string &memberName);

std::optional<InjectedMemberBinding>
resolveInjectedMemberBinding(TypeTable *typeMgr, HIRExpr *receiver,
                             const std::string &memberName);

void
rejectBareFunctionStorage(TypeClass *type, AstVarDef *node);

void
rejectOpaqueStructStorage(TypeClass *type, AstVarDef *node);

void
rejectConstVariableStorage(TypeClass *type, AstVarDef *node);

void
rejectUninitializedFunctionPointerValueStorage(TypeClass *type,
                                               AstVarDef *node);

}  // namespace analysis_impl
}  // namespace lona
