

#include "visitor/base.hh"

namespace lona {

class TypeScanner : public AstVisitorAny {
    TypeManger* typeMgr = nullptr;

public:
    TypeScanner(TypeManger* typeMgr) : typeMgr(typeMgr) {}
    using AstVisitorAny::visit;

    BaseVariable* visit(AstProgram* node) override {
        node->body->accept(*this);
        return nullptr;
    }

    BaseVariable* visit(AstStatList* node) override {
        for (auto it = node->getBody().begin(); it != node->getBody().end();
             it++) {
            (*it)->accept(*this);
        }
        return nullptr;
    }

    BaseVariable* visit(AstFuncDecl* node) override { return nullptr; }
};

void
scanningType(TypeManger* typeMgr, AstNode* root) {
    TypeScanner scanner(typeMgr);
    scanner.AstVisitor::visit(root);
};

}  // namespace lona
