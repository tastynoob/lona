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
        : scope(scope), builder(scope->builder) {
        this->node = node;
        this->visit(node->body);
    }

    StructType* getStruct() {
        std::string struct_name = "struct." + node->name;
        auto structTy = llvm::StructType::create(builder.getContext(),
                                                 llvmmembers, struct_name);
        auto typesize =
            scope->module.getDataLayout().getTypeSizeInBits(structTy) / 8;
        return new StructType(structTy, std::move(members), struct_name,
                              typesize);
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
        if (!node->typeHelper) {
            throw "Not allowed auto infer type";
        }

        // add struct member
        auto& name = node->field;
        auto type = scope->getType(node->typeHelper);

        llvmmembers.push_back(type->llvmType);
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