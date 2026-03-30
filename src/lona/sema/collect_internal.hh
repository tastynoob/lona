#pragma once

#include "lona/ast/astnode.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/module/module_interface.hh"
#include "lona/sym/func.hh"
#include "lona/type/scope.hh"
#include "parser.hh"
#include <llvm-18/llvm/ADT/StringMap.h>
#include <llvm-18/llvm/ADT/StringSet.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lona {

namespace collect_decl_impl {

enum class TopLevelDeclKind {
    StructType,
    Function,
    Global,
};

void
recordTopLevelDeclName(
    std::unordered_map<std::string, std::pair<TopLevelDeclKind, location>>
        &seen,
    const std::string &name, TopLevelDeclKind kind, const location &loc);

std::vector<string>
extractParamNames(AstFuncDecl *node);

std::vector<BindingKind>
extractParamBindingKinds(AstFuncDecl *node, bool withImplicitSelf = false);

TypeClass *
methodReceiverPointeeType(TypeTable *typeMgr, StructType *methodParent,
                          AccessKind receiverAccess);

TypeClass *
interfaceMethodReceiverPointeeType(ModuleInterface *interface,
                                   StructType *methodParent,
                                   AccessKind receiverAccess);

void
validateFunctionReceiverAccess(AstFuncDecl *node, StructType *methodParent);

void
rejectOpaqueStructByValue(TypeClass *type, TypeNode *typeNode,
                          const location &loc, const std::string &context);

void
validateStructDeclShape(AstStructDecl *node);

std::string
describeStructFieldSyntax(AstVarDecl *fieldDecl);

void
validateEmbeddedStructField(AstStructDecl *structDecl, AstVarDecl *fieldDecl,
                            TypeClass *fieldType);

void
insertStructMember(AstStructDecl *structDecl, AstVarDecl *fieldDecl,
                   TypeClass *fieldType,
                   llvm::StringMap<StructType::ValueTy> &members,
                   llvm::StringMap<AccessKind> &memberAccess,
                   llvm::StringSet<> &embeddedMembers,
                   std::unordered_map<std::string, location> &seenMembers,
                   int &nextMemberIndex);

void
validateStructFieldType(AstStructDecl *structDecl, AstVarDecl *fieldDecl,
                        TypeClass *fieldType);

TypeClass *
resolveTypeNode(TypeTable *typeMgr, const CompilationUnit *unit,
                TypeNode *node);

void
rejectBareFunctionType(TypeClass *type, TypeNode *node,
                       const std::string &context,
                       const location &loc = location());

TypeTable *
requireTypeTable(Scope *scope);

void
declareModuleNamespace(Scope &scope, const CompilationUnit &unit);

void
validateExternCFunctionSignature(AstFuncDecl *node, StructType *methodParent,
                                 const std::vector<TypeClass *> &argTypes,
                                 TypeClass *retType);

StructType *
declareStructType(TypeTable *typeMgr, AstStructDecl *node,
                  CompilationUnit *unit = nullptr,
                  bool exportNamespace = false);

Function *
declareFunction(Scope &scope, TypeTable *typeMgr, AstFuncDecl *node,
                StructType *methodParent, CompilationUnit *unit = nullptr,
                bool exportNamespace = false);

}  // namespace collect_decl_impl

namespace collect_interface_impl {

void
ensureUnitInterfaceCollected(CompilationUnit &unit);

}  // namespace collect_interface_impl

}  // namespace lona
