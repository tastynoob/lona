#pragma once

#include "lona/ast/astnode.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/module/module_interface.hh"
#include <cstdint>
#include <llvm-18/llvm/ADT/StringRef.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace lona::appliedstructinstantiation {

using GenericArgMap = std::unordered_map<std::string, TypeClass *>;

std::string
buildAppliedTypeName(const std::string &baseName,
                     const std::vector<TypeClass *> &args);

GenericArgMap
bindGenericArgs(const ModuleInterface::TypeDecl &typeDecl,
                const std::vector<TypeClass *> &appliedTypeArgs,
                const std::string &bugContext);

AstStructDecl *
findStructDeclInUnit(const CompilationUnit &unit, llvm::StringRef localName);

class TypeOps {
public:
    virtual ~TypeOps() = default;

    virtual TypeClass *resolveFallbackType(
        TypeNode *node, const CompilationUnit &lookupUnit) const = 0;
    virtual const ModuleInterface::TypeDecl *resolveVisibleTypeDecl(
        BaseTypeNode *base, const CompilationUnit &lookupUnit) const = 0;
    virtual TypeClass *instantiateAppliedStructType(
        const ModuleInterface::TypeDecl &typeDecl,
        std::vector<TypeClass *> argTypes,
        const CompilationUnit &lookupUnit) const = 0;

    virtual TypeClass *createAnyType() const = 0;
    virtual TypeClass *createConstType(TypeClass *baseType) const = 0;
    virtual TypeClass *createPointerType(TypeClass *baseType) const = 0;
    virtual TypeClass *createIndexablePointerType(
        TypeClass *baseType) const = 0;
    virtual TypeClass *createArrayType(
        TypeClass *baseType, std::vector<AstNode *> dimensions) const = 0;
    virtual TypeClass *createTupleType(
        const std::vector<TypeClass *> &itemTypes) const = 0;
    virtual TypeClass *createFunctionPointerType(
        const std::vector<TypeClass *> &argTypes, TypeClass *retType,
        std::vector<BindingKind> argBindingKinds) const = 0;
};

class MaterializationOps : public TypeOps {
public:
    virtual TypeClass *receiverPointeeType(StructType *structType,
                                           AccessKind receiverAccess) const = 0;
    virtual FuncType *createMethodFunctionType(
        const std::vector<TypeClass *> &argTypes, TypeClass *retType,
        const std::vector<BindingKind> &paramBindingKinds) const = 0;
};

TypeClass *
substituteTemplateType(TypeNode *node, const GenericArgMap &genericArgs,
                       const location &loc, const std::string &context,
                       const CompilationUnit &lookupUnit, const TypeOps &ops);

void
materializeStructLayoutAndMethods(const ModuleInterface::TypeDecl &typeDecl,
                                  StructType *structType,
                                  const GenericArgMap &genericArgs,
                                  const CompilationUnit &templateOwnerUnit,
                                  const MaterializationOps &ops);

}  // namespace lona::appliedstructinstantiation
