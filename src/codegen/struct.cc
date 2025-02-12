#include "base.hh"
#include "type/scope.hh"

namespace lona {

class StructVisitor : public AstVisitorAny {
    llvm::IRBuilder<>& builder;
    Scope* scope;
    std::vector<llvm::Type*> llvmmembers;
    llvm::StringMap<std::pair<TypeClass*, int>> members;
    AstStructDecl* node;

public:
    StructVisitor(Scope* scope, AstStructDecl* node)
        : scope(scope), builder(scope->getBuilder()) {
        this->node = node;
        this->visit(node->body);
    }

    StructType* getStruct() {
        auto structTy = llvm::StructType::create(
            builder.getContext(), llvmmembers, "struct." + node->name);
        return new StructType(structTy, std::move(members));
    }

    using AstVisitorAny::visit;

    Object* visit(AstStatList* node) override {
        for (auto it = node->getBody().begin(); it != node->getBody().end();
             it++) {
            (*it)->accept(*this);
        }
        return nullptr;
    }

    Object* visit(AstVarDecl* node) override {
        // add struct member
        auto& name = node->field;
        auto type = scope->getType(node->typeHelper);

        llvmmembers.push_back(type->getllvmType());
        members.insert({name, {type, llvmmembers.size() - 1}});

        return nullptr;
    }

    Object* visit(AstFuncDecl* node) override { return nullptr; }
};

StructType*
createStruct(Scope* scope, AstStructDecl* node) {
    StructVisitor visitor(scope, node);
    return visitor.getStruct();
}

}