#pragma once

#include "ast/astnode.hh"
#include "type/typeclass.hh"

namespace lona {

class AstVisitor {
#define DEF_VISIT(classname)                                      \
    virtual BaseVariable *visit(classname *node) {                \
        throw std::runtime_error("Not implemented: " #classname); \
    }

public:
    BaseVariable *visit(AstNode *node) { return node->accept(*this); }
    DEF_VISIT(AstProgram)
    DEF_VISIT(AstConst)
    DEF_VISIT(AstField)
    DEF_VISIT(AstAssign)
    DEF_VISIT(AstBinOper)
    DEF_VISIT(AstUnaryOper)
    DEF_VISIT(AstVarDecl)
    DEF_VISIT(AstStatList)
    DEF_VISIT(AstFuncDecl)
    DEF_VISIT(AstRet)
    DEF_VISIT(AstIf)
    DEF_VISIT(AstFieldCall)
};


class AstVisitorAny : public AstVisitor {
#undef DEF_VISIT
#define DEF_VISIT(classname) \
    BaseVariable *visit(classname *node) override { return nullptr; }

public:
    DEF_VISIT(AstProgram)
    DEF_VISIT(AstConst)
    DEF_VISIT(AstField)
    DEF_VISIT(AstAssign)
    DEF_VISIT(AstBinOper)
    DEF_VISIT(AstUnaryOper)
    DEF_VISIT(AstVarDecl)
    DEF_VISIT(AstStatList)
    DEF_VISIT(AstFuncDecl)
    DEF_VISIT(AstRet)
    DEF_VISIT(AstIf)
    DEF_VISIT(AstFieldCall)
};

class TypeManger;
class AstNode;

void
scanningType(TypeManger *typeMgr, AstNode *root);

}  // namespace lona
