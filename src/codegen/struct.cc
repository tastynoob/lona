#include "base.hh"
#include "type/scope.hh"

namespace lona {

class StructVisitor : public AstVisitorAny {
    llvm::IRBuilder<>& builder;
    Scope* scope;
    std::vector<llvm::Type*> llvmmembers;
    llvm::StringMap<std::pair<TypeClass*, int>> members;
    llvm::StringMap<Method*> funcs;
    AstStructDecl* node;

    StructType* lostructTy = nullptr;

public:
    StructVisitor(Scope* scope, AstStructDecl* node)
        : scope(scope), builder(scope->builder) {

        std::string struct_name = "struct." + node->name;
        auto structTy =
            llvm::StructType::create(builder.getContext(), struct_name);
        lostructTy = new StructType(structTy, struct_name);

        scope->addType(node->name, lostructTy);

        this->node = node;
        this->visit(node->body);
    }

    StructType* getStruct() {
        ((llvm::StructType*)lostructTy->llvmType)->setBody(llvmmembers);

        auto typesize = scope->module.getDataLayout().getTypeSizeInBits(
                            lostructTy->llvmType) /
                        8;
        lostructTy->typeSize = typesize;
        lostructTy->setMembers(std::move(members));
        lostructTy->setFuncs(std::move(funcs));
        return lostructTy;
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

    Object* visit(AstFuncDecl* node) override {
        auto func = createFunc(*scope, node, lostructTy);

        scope->addObj(func->getType()->full_name, func);
        funcs.insert({node->name, func});

        return nullptr;
    }
};

StructType*
createStruct(Scope* scope, AstStructDecl* node) {
    StructVisitor visitor(scope, node);
    return visitor.getStruct();
}

}