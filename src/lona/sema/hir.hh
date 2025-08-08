#pragma once
#include "../type/type.hh"
#include "lona/ast/astnode.hh"
#include "lona/obj/value.hh"
#include "lona/type/scope.hh"
#include <vector>


// lona's High Intermediate Representation (HIR)
namespace lona {

class HIRNode;
class HIRValue;
class HIRBinOper;
class HIRUnaryOper;
class HIRAssign;
class HIRIf;
class HIRFor;
class HIRRet;
class HIRFuncCall;


class HIRVisitor{
#define DEF_VISIT(classname) \
    virtual void visit(classname *node) {}

public:
    DEF_VISIT(HIRNode)
    DEF_VISIT(HIRValue)
    DEF_VISIT(HIRBinOper)
    DEF_VISIT(HIRUnaryOper)
    DEF_VISIT(HIRAssign)
    DEF_VISIT(HIRIf)
    DEF_VISIT(HIRFor)
    DEF_VISIT(HIRRet)
    DEF_VISIT(HIRFuncCall)
};

class HIRNode {
    HIRNode* cfg_next = nullptr;  // next node in CFG    

    void setCFGNextNode(HIRNode* node) {
        cfg_next = node;
    }

    HIRNode* getValidCFGNode() {
        if (cfg_next) {
            return cfg_next;
        }
        return this;
    }
    virtual void accept(HIRVisitor &visitor) = 0;
};

class HIRExpr : public HIRNode {
public:
    ObjectPtr res = nullptr;  // expression value
};

class HIRStmt : public HIRNode {}

#define DEF_ACCEPT() \
    void accept(HIRVisitor &visitor) override { \
        visitor.visit(this); \
    }

class HIRBinOper : public HIRExpr {
    token_type op;
    ObjectPtr left;
    ObjectPtr right;
public:
    HIRBinOper(token_type op, ObjectPtr left, ObjectPtr right)
        : op(op), left(left), right(right) {}
    
    DEF_ACCEPT()
};

class HIRUnaryOper : public HIRExpr {
    token_type op;
    ObjectPtr expr;
public:
    HIRUnaryOper(token_type op, ObjectPtr expr)
        : op(op), expr(expr) {}

    DEF_ACCEPT()
};

class HIRFuncCall : public HIRExpr {
    ObjectPtr func;
    std::vector<ObjectPtr> args;
    ObjectPtr retValue = nullptr;  // optional return value
public:
    HIRFuncCall(ObjectPtr func, std::vector<ObjectPtr> args)
        : func(func), args(args) {}
    ObjectPtr getFunc() const { return func; }

    DEF_ACCEPT()
};

class HIRAssign : public HIRStmt {
    ObjectPtr right;
public:
    HIRAssign(ObjectPtr left, ObjectPtr right)
        : right(right) {
            this->res = left;
        }

    DEF_ACCEPT()
};

class HIRIf : public HIRStmt {
    ObjectPtr condval;
    std::vector<HIRNode*> thenBranch;
    std::vector<HIRNode*> elseBranch;
public:
    HIRIf(ObjectPtr condval)
        : condval(condval) {}

    auto& getThenNodes() {
        return thenBranch;
    }

    auto& getElseNodes() {
        return elseBranch;
    }

    void pushThenNode(HIRNode *node) {
        thenBranch.push_back(node);
    }
    void pushElseNode(HIRNode *node) {
        elseBranch.push_back(node);
    }

    DEF_ACCEPT()
};

class HIRFor : public HIRStmt {
    ObjectPtr condVal;
    std::vector<HIRNode*> body;
public:
    HIRFor(ObjectPtr condVal)
        : condVal(condVal) {}

    auto& getBodyNodes() {
        return body;
    }

    void pushBodyNode(HIRNode *node) {
        body.push_back(node);
    }

    DEF_ACCEPT()
};

class HIRRet : public HIRStmt {
    ObjectPtr *retValue;
public:
    HIRRet(ObjectPtr *retValue) : retValue(retValue) {}

    DEF_ACCEPT()
};

class HIRFunc : public HIRNode {
    ObjectPtr funcObj;
    std::vector<ObjectPtr> args;
    ObjectPtr retValue = nullptr;;

    std::vector<ObjectPtr> allocatedVars;  // variables allocated in this function
    std::vector<HIRNode*> body;  // function body in HIR
public:
    auto& getBodyNodes() {
        return body;
    }
    void pushBodyNode(HIRNode *node) {
        body.push_back(node);
    }
    void pushAllocatedVar(ObjectPtr var) {
        allocatedVars.push_back(var);
    }
    void pushArg(ObjectPtr arg) {
        args.push_back(arg);
    }
    void setReturnValue(ObjectPtr ret) {
        retValue = ret;
    }

    DEF_ACCEPT()
};

}