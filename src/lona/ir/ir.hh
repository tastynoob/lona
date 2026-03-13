#pragma once
#include "../type/type.hh"
#include "lona/ast/astnode.hh"
#include "lona/sym/object.hh"
#include "lona/type/scope.hh"
#include <cassert>
#include <cstddef>
#include <list>
#include <llvm-18/llvm/IR/BasicBlock.h>
#include <llvm-18/llvm/IR/IRBuilder.h>
#include <llvm-18/llvm/IR/Instruction.h>
#include <vector>


// lona's High Intermediate Representation (IR)
namespace lona {

class IRNode;
class IRTerminator;

class IRValue;
class IRBinOper;
class IRUnaryOper;
class IRAssign;
class IRIf;
class IRFor;
class IRRet;
class IRFuncCall;
class IRBlock;


class IRVisitor{
#undef DEF_VISIT
#define DEF_VISIT(classname) \
    virtual void visit(classname *node) {}

public:
    DEF_VISIT(IRNode)
    DEF_VISIT(IRValue)
    DEF_VISIT(IRBinOper)
    DEF_VISIT(IRUnaryOper)
    DEF_VISIT(IRAssign)
    DEF_VISIT(IRIf)
    DEF_VISIT(IRFor)
    DEF_VISIT(IRRet)
    DEF_VISIT(IRFuncCall)
};

#define DEF_ACCEPT() \
    void accept(IRVisitor &visitor) override { \
        visitor.visit(this); \
    }

class IRNode {
public:
    template<class T>
    T* as() {
        return dynamic_cast<T *>(this);
    }

    virtual void accept(IRVisitor &visitor) = 0;
};

typedef std::vector<IRNode*> IRNodes;

class IRStmt : public IRNode {};

class IRExpr : public IRStmt {
    TypeClass* type;
public:
    TypeClass* getType() const { return type; }
    void setType(TypeClass* type) { this->type = type; }
};

class IRBinOper : public IRExpr {
    token_type op;
    IRNode* left;
    IRNode* right;
public:
    IRBinOper(token_type op, IRNode* left, IRNode* right)
        : op(op), left(left), right(right) {}

    DEF_ACCEPT()
};

class IRUnaryOper : public IRExpr {
    token_type op;
    IRNode* expr;
public:
    IRUnaryOper(token_type op, IRNode* expr)
        : op(op), expr(expr) {}

    DEF_ACCEPT()
};

class IRFuncCall : public IRExpr {
    ObjectPtr func;
    std::vector<ObjectPtr> args;
public:
    IRFuncCall(ObjectPtr func, std::vector<ObjectPtr> args)
        : func(func), args(args) {}
    ObjectPtr getFunc() const { return func; }

    DEF_ACCEPT()
};

class IRAssign : public IRStmt {
    ObjectPtr left;  // left value
    ObjectPtr right;
public:
    IRAssign(ObjectPtr left, ObjectPtr right)
        : left(left), right(right) {}

    DEF_ACCEPT()
};

class IRIf : public IRStmt {
    IRNodes thenBB;
    IRNodes elseBB;
    IRNode* cond;
public:
    IRIf(IRNode* cond)
        : cond(cond) {}

    bool hasElseBlock() const {
        return elseBB.size() > 0;
    }

    IRNodes* getThenBlock() {
        return &thenBB;
    }

    IRNodes* getElseBlock() {
        return &elseBB;
    }

    DEF_ACCEPT()
};

// class IRFor : public IRTerminator {
//     ObjectPtr condVal;
//     IRBlock condBB;  // condition block
//     IRBlock body;
// public:
//     IRFor(ObjectPtr condVal)
//         : condVal(condVal) {}

//     auto* getCondBlock() {
//         return &condBB;
//     }

//     auto* getBodyNodes() {
//         return &body;
//     }

//     DEF_ACCEPT()
// };

class IRRet : public IRStmt {
    ObjectPtr retValue;
public:
    IRRet(ObjectPtr retValue) : retValue(retValue) {}

    DEF_ACCEPT()
};

class IRFunc : public IRNode {
    ObjectPtr funcObj;
    std::vector<ObjectPtr> args;
    ObjectPtr retValue = nullptr;;

    std::vector<ObjectPtr> allocatedVars;  // variables allocated in this function
    std::vector<IRBlock*> body;  // function body in IR
public:
    auto& getBlocks() {
        return body;
    }
    void pushBlock(IRBlock *node) {
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