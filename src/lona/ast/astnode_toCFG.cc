
#include "../ast/astnode.hh"
#include "../util/cfg.hh"

namespace lona {

void
AstNode::toCFG(CFGChecker &checker) {
    auto name = checker.nextName();
    checker.visit(this, name);
    if (cfg_next) {
        auto next = cfg_next->getValidCFGNode();
        if (!checker.isVisited(next)) cfg_next->toCFG(checker);
        checker.link(this, next);
    } else {
        checker.addEndNode(this);
    }
}

void
AstStatList::toCFG(CFGChecker &checker) {
    if (!body.empty()) {
        body.front()->toCFG(checker);
    }
}

void
AstIf::toCFG(CFGChecker &checker) {
    auto ifname = checker.nextName("if_");
    checker.visit(this, ifname);

    if (then) {
        auto next = then->getValidCFGNode();
        if (!checker.isVisited(next)) next->toCFG(checker);
        checker.link(this, next);
    } else {
        checker.addEndNode(then);
    }

    if (els) {
        auto next = els->getValidCFGNode();
        if (!checker.isVisited(next)) next->toCFG(checker);
        checker.link(this, next);
    } else if (cfg_next) {
        if (!checker.isVisited(cfg_next)) cfg_next->toCFG(checker);
        checker.link(this, cfg_next);
    } else {
        checker.addEndNode(this);
    }
}

void
AstFor::toCFG(CFGChecker &checker) {
    auto forname = checker.nextName("for_");
    checker.visit(this, forname);

    auto next = body->getValidCFGNode();
    if (next) {
        if (!checker.isVisited(next)) next->toCFG(checker);
        checker.link(this, next);
    }

    if (els) {
        auto elseNext = els->getValidCFGNode();
        if (elseNext) {
            if (!checker.isVisited(elseNext)) elseNext->toCFG(checker);
            checker.link(this, elseNext);
        } else {
            checker.addEndNode(this);
        }
    } else if (cfg_next) {
        auto next = cfg_next->getValidCFGNode();
        if (!checker.isVisited(next)) cfg_next->toCFG(checker);
        checker.link(this, next);
    } else {
        checker.addEndNode(this);
    }
}

void
AstRet::toCFG(CFGChecker &checker) {
    assert(!cfg_next);
    auto name = checker.nextName("ret_");
    checker.visit(this, name);
    checker.addEndNode(this);
}

void
AstBreak::toCFG(CFGChecker &checker) {
    auto name = checker.nextName("break_");
    checker.visit(this, name);
    checker.addEndNode(this);
}

void
AstContinue::toCFG(CFGChecker &checker) {
    auto name = checker.nextName("continue_");
    checker.visit(this, name);
    checker.addEndNode(this);
}

} 
