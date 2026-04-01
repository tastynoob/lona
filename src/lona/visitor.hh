#pragma once

#include "ast/astnode.hh"
#include "module/compilation_unit.hh"
#include "type/type.hh"

namespace lona {

class ModuleGraph;

class AstVisitor {
#define DEF_VISIT(classname)                                      \
    virtual Object *visit(classname *node) {                      \
        throw std::runtime_error("Not implemented: " #classname); \
    }

public:
    Object *visit(AstNode *node) { return node->accept(*this); }
    DEF_VISIT(AstProgram)
    DEF_VISIT(AstTagNode)
    DEF_VISIT(AstConst)
    DEF_VISIT(AstField)
    DEF_VISIT(AstFuncRef)
    DEF_VISIT(AstAssign)
    DEF_VISIT(AstBinOper)
    DEF_VISIT(AstUnaryOper)
    DEF_VISIT(AstRefExpr)
    DEF_VISIT(AstTupleLiteral)
    DEF_VISIT(AstBraceInitItem)
    DEF_VISIT(AstBraceInit)
    DEF_VISIT(AstNamedCallArg)
    DEF_VISIT(AstTypeApply)
    DEF_VISIT(AstStructDecl)
    DEF_VISIT(AstTraitDecl)
    DEF_VISIT(AstTraitImplDecl)
    DEF_VISIT(AstGlobalDecl)
    DEF_VISIT(AstImport)
    DEF_VISIT(AstVarDecl)
    DEF_VISIT(AstVarDef)
    DEF_VISIT(AstStatList)
    DEF_VISIT(AstFuncDecl)
    DEF_VISIT(AstRet)
    DEF_VISIT(AstBreak)
    DEF_VISIT(AstContinue)
    DEF_VISIT(AstIf)
    DEF_VISIT(AstFor)
    DEF_VISIT(AstCastExpr)
    DEF_VISIT(AstSizeofExpr)
    DEF_VISIT(AstFieldCall)
    DEF_VISIT(AstDotLike)
};

class AstVisitorAny : public AstVisitor {
#undef DEF_VISIT
#define DEF_VISIT(classname) \
    Object *visit(classname *node) override { return nullptr; }

public:
    using AstVisitor::visit;
    DEF_VISIT(AstProgram)
    DEF_VISIT(AstTagNode)
    DEF_VISIT(AstConst)
    DEF_VISIT(AstField)
    DEF_VISIT(AstFuncRef)
    DEF_VISIT(AstAssign)
    DEF_VISIT(AstBinOper)
    DEF_VISIT(AstUnaryOper)
    DEF_VISIT(AstRefExpr)
    DEF_VISIT(AstTupleLiteral)
    DEF_VISIT(AstBraceInitItem)
    DEF_VISIT(AstBraceInit)
    DEF_VISIT(AstNamedCallArg)
    DEF_VISIT(AstTypeApply)
    DEF_VISIT(AstStructDecl)
    DEF_VISIT(AstTraitDecl)
    DEF_VISIT(AstTraitImplDecl)
    DEF_VISIT(AstGlobalDecl)
    DEF_VISIT(AstImport)
    DEF_VISIT(AstVarDecl)
    DEF_VISIT(AstVarDef)
    DEF_VISIT(AstStatList)
    DEF_VISIT(AstFuncDecl)
    DEF_VISIT(AstRet)
    DEF_VISIT(AstBreak)
    DEF_VISIT(AstContinue)
    DEF_VISIT(AstIf)
    DEF_VISIT(AstFor)
    DEF_VISIT(AstCastExpr)
    DEF_VISIT(AstSizeofExpr)
    DEF_VISIT(AstFieldCall)
    DEF_VISIT(AstDotLike)
};

class Scope;
class AstNode;
class StructType;
class Function;
class HIRModule;

Function *
createFunc(Scope &scope, AstFuncDecl *root, StructType *parent = nullptr);

void
compileModule(Scope *global, AstNode *root, bool emitDebugInfo = false);

void
scanningType(Scope *global, AstNode *root);

void
collectUnitDeclarations(Scope *global, CompilationUnit &unit,
                        bool exportNamespace);

void
defineUnitGlobals(Scope *global, CompilationUnit &unit);

void
emitHIRModule(Scope *global, HIRModule *module, bool emitDebugInfo = false,
              const std::string &primarySourcePath = std::string(),
              const CompilationUnit *unit = nullptr,
              const ModuleGraph *moduleGraph = nullptr);

StructType *
createStruct(Scope *scope, AstStructDecl *node);

}  // namespace lona
