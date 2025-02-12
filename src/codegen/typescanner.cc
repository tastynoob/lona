

#include "codegen/base.hh"
#include "type/scope.hh"

namespace lona {

class TypeScanner : public AstVisitorAny {
    llvm::IRBuilder<>& builder;
    Scope* global = nullptr;

public:
    TypeScanner(Scope* global)
        : global(global), builder(global->getBuilder()) {}
    using AstVisitorAny::visit;

    Object* visit(AstProgram* node) override {
        node->body->accept(*this);
        return nullptr;
    }

    Object* visit(AstStatList* node) override {
        for (auto it = node->getBody().begin(); it != node->getBody().end();
             it++) {
            (*it)->accept(*this);
        }
        return nullptr;
    }

    Object* visit(AstFuncDecl* node) override {
        // create function type
        auto func = node->createFunc(*global);
        global->addObj(node->name, func);

        return nullptr;
    }
};

void
scanningType(Scope* global, AstNode* root) {
    TypeScanner scanner(global);
    scanner.AstVisitor::visit(root);
};

}  // namespace lona
