#pragma once

#include "ast/astnode.hh"
#include "type/type.hh"

namespace lona {

class AstVisitor {
#define DEF_VISIT(classname)                                      \
    virtual Object *visit(classname *node) {                      \
        throw std::runtime_error("Not implemented: " #classname); \
    }

public:
    Object *visit(AstNode *node) { return node->accept(*this); }
    DEF_VISIT(AstProgram)
    DEF_VISIT(AstConst)
    DEF_VISIT(AstField)
    DEF_VISIT(AstAssign)
    DEF_VISIT(AstBinOper)
    DEF_VISIT(AstUnaryOper)
    DEF_VISIT(AstStructDecl)
    DEF_VISIT(AstVarDecl)
    DEF_VISIT(AstStatList)
    DEF_VISIT(AstFuncDecl)
    DEF_VISIT(AstRet)
    DEF_VISIT(AstIf)
    DEF_VISIT(AstFieldCall)
    DEF_VISIT(AstSelector)
};

class AstVisitorAny : public AstVisitor {
#undef DEF_VISIT
#define DEF_VISIT(classname) \
    Object *visit(classname *node) override { return nullptr; }

public:
    using AstVisitor::visit;
    DEF_VISIT(AstProgram)
    DEF_VISIT(AstConst)
    DEF_VISIT(AstField)
    DEF_VISIT(AstAssign)
    DEF_VISIT(AstBinOper)
    DEF_VISIT(AstUnaryOper)
    DEF_VISIT(AstStructDecl)
    DEF_VISIT(AstVarDecl)
    DEF_VISIT(AstStatList)
    DEF_VISIT(AstFuncDecl)
    DEF_VISIT(AstRet)
    DEF_VISIT(AstIf)
    DEF_VISIT(AstFieldCall)
    DEF_VISIT(AstSelector)
};

class Scope;
class AstNode;
class StructType;
class Functional;

Functional *
createFunc(Scope &scope, AstFuncDecl *root);

void
scanningType(Scope *global, AstNode *root);

StructType *
createStruct(Scope *scope, AstStructDecl *node);

}  // namespace lona
