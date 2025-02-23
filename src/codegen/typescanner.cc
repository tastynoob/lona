

#include "codegen/base.hh"
#include "type/scope.hh"

namespace lona {

class TypeScanner : public AstVisitorAny {
    llvm::IRBuilder<>& builder;
    Scope* global = nullptr;

public:
    TypeScanner(Scope* global) : global(global), builder(global->builder) {}
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

    Object* visit(AstStructDecl* node) override {
        auto structTy = createStruct(global, node);
        // global->addType(node->name, structTy);
        return nullptr;
    }

    Object* visit(AstFuncDecl* node) override {
        // create function type
        auto func = createFunc(*global, node);
        global->addObj(node->name, func);

        return nullptr;
    }
};

Functional*
createFunc(Scope& scope, AstFuncDecl* root, StructType* parent) {
    // create function type
    auto& builder = scope.builder;
    std::vector<llvm::Type*> args;
    std::vector<TypeClass*> loargs;
    llvm::Type* retType = builder.getVoidTy();
    TypeClass* loretType = nullptr;

    if (root->retType) {
        loretType = scope.getType(root->retType);
        retType = loretType->llvmType;
        if (loretType->isPassByPointer()) {
            args.push_back(retType->getPointerTo());
            loargs.push_back(loretType->getPointerType(&scope));
        }
    }

    if (parent) {
        args.push_back(parent->llvmType->getPointerTo());
        loargs.push_back(parent);
    }

    if (root->args)
        for (auto it : *root->args) {
            auto decl = dynamic_cast<AstVarDecl*>(it);
            assert(decl);
            if (!decl->typeHelper) {
                throw "type of argument is not allowed auto infer";
            }
            auto type = scope.getType(decl->typeHelper);
            args.push_back(type->llvmType);
            loargs.push_back(type);
        }
    llvm::FunctionType* funcType =
        llvm::FunctionType::get(retType, args, false);

    std::string func_name;

    if (parent) {
        func_name = parent->full_name + '.' + root->name;
    } else {
        func_name = scope.getName() + '.' + root->name;
    }

    // create function
    llvm::Function* func = llvm::Function::Create(
        funcType, llvm::Function::ExternalLinkage, func_name, scope.module);

    return new Functional(func, new FuncType(funcType, std::move(loargs),
                                             loretType, func_name, 8));
}

void
scanningType(Scope* global, AstNode* root) {
    TypeScanner scanner(global);
    scanner.AstVisitor::visit(root);
};

}  // namespace lona
