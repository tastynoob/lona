#pragma once

#include "lona/ir/ir.hh"

namespace lona {

class IRValue : public IRNode {
    ObjectPtr value;

public:
    explicit IRValue(ObjectPtr value) : value(value) {}
    ObjectPtr getValue() const { return value; }

    void accept(IRVisitor &visitor) override {
        visitor.visit(this);
    }
};

using HIRNode = IRNode;
using HIRFunc = IRFunc;
using HIRAssign = IRAssign;

class IRBlock : public IRNode {
    IRNodes body;

public:
    IRNodes *getBody() { return &body; }
    void push(IRNode *node) { body.push_back(node); }

    void accept(IRVisitor &visitor) override {
        visitor.visit(static_cast<IRNode *>(this));
    }
};

using HIRBlock = IRBlock;

class HIRIf : public IRIf {
public:
    explicit HIRIf(ObjectPtr cond)
        : IRIf(new IRValue(cond)) {}

    explicit HIRIf(IRNode *cond)
        : IRIf(cond) {}

    void pushThenNode(IRNode *node) {
        getThenBlock()->push_back(node);
    }

    void pushElseNode(IRNode *node) {
        getElseBlock()->push_back(node);
    }
};

}  // namespace lona
