#include "../visitor.hh"
#include "../type/buildin.hh"
#include "../type/scope.hh"
#include "lona/ast/astnode.hh"
#include "lona/sym/func.hh"
#include "lona/type/type.hh"
#include <cassert>
#include <cstddef>
#include <list>
#include <llvm-18/llvm/IR/DerivedTypes.h>
#include <llvm-18/llvm/IR/Type.h>
#include <string>
#include <vector>

namespace lona {
namespace {

TypeTable *
requireTypeTable(Scope *scope) {
    assert(scope);
    auto *typeMgr = scope->types();
    assert(typeMgr);
    return typeMgr;
}

StructType *
declareStructType(TypeTable *typeMgr, AstStructDecl *node) {
    auto *existing = typeMgr->getType(node->name);
    if (existing != nullptr) {
        return existing->as<StructType>();
    }

    auto struct_name = node->name;
    auto *structTy = llvm::StructType::create(
        typeMgr->getContext(),
        llvm::StringRef(struct_name.tochara(), struct_name.size()));
    auto *lostructTy = new StructType(structTy, struct_name);

    typeMgr->addType(struct_name, lostructTy);
    return lostructTy;
}

Function *
declareFunction(Scope &scope, TypeTable *typeMgr, AstFuncDecl *node) {
    auto &func_name = node->name;
    if (auto *existing = scope.getObj(func_name)) {
        return existing->as<Function>();
    }

    std::vector<TypeClass *> loargs;
    std::vector<llvm::Type *> args;

    TypeClass *loretType = nullptr;
    llvm::Type *retType = llvm::Type::getVoidTy(typeMgr->getContext());

    std::string funcType_name = "f";
    if (node->retType) {
        loretType = typeMgr->getType(node->retType);
        assert(loretType);
        retType = loretType->llvmType;
        funcType_name += "_";
        funcType_name.append(loretType->full_name.tochara(),
                             loretType->full_name.size());

        if (loretType->shouldReturnByPointer()) {
            auto *sroa_retType = typeMgr->createPointerType(loretType);
            loargs.push_back(sroa_retType);
            args.push_back(sroa_retType->llvmType);
        }
    }

    if (node->args) {
        for (auto *arg : *node->args) {
            assert(arg->is<AstVarDecl>());
            auto *varDecl = arg->as<AstVarDecl>();
            auto *type = typeMgr->getType(varDecl->typeNode);
            assert(type);
            loargs.push_back(type);
            args.push_back(type->llvmType);
            funcType_name += ".";
            funcType_name.append(type->full_name.tochara(),
                                 type->full_name.size());
        }
    }

    TypeClass *lofuncType = typeMgr->getType(funcType_name);
    if (!lofuncType) {
        auto *llfuncType = llvm::FunctionType::get(retType, args, false);
        lofuncType = new FuncType(
            llfuncType, std::move(loargs), loretType, func_name, 0);
        typeMgr->addType(funcType_name, lofuncType);
    }

    auto *llfunc = llvm::Function::Create(
        (llvm::FunctionType *)lofuncType->llvmType,
        llvm::Function::ExternalLinkage,
        llvm::Twine(func_name.tochara()),
        typeMgr->getModule());
    auto *lofunc = new Function(llfunc, lofuncType->as<FuncType>());

    scope.addObj(func_name, lofunc);
    return lofunc;
}

}  // namespace

class StructVisitor : public AstVisitorAny {
    TypeTable *typeMgr;

    std::vector<llvm::Type *> llvmmembers;
    llvm::StringMap<StructType::ValueTy> members;

    using AstVisitorAny::visit;

    Object *visit(AstStatList *node) override {
        for (auto it = node->getBody().begin(); it != node->getBody().end();
             it++) {
            (*it)->accept(*this);
        }
        return nullptr;
    }

    Object *visit(AstVarDecl *node) override {
        auto &name = node->field;
        auto *type = typeMgr->getType(node->typeNode);
        assert(type);

        llvmmembers.push_back(type->llvmType);
        members.insert({llvm::StringRef(name.tochara(), name.size()),
                        {type, static_cast<int>(llvmmembers.size() - 1)}});

        return nullptr;
    }

public:
    StructVisitor(TypeTable *typeMgr, AstStructDecl *node) : typeMgr(typeMgr) {
        auto *lostructTy = declareStructType(typeMgr, node);
        assert(lostructTy);
        assert(lostructTy->llvmType);

        if (!lostructTy->isOpaque()) {
            return;
        }

        this->visit(node->body);

        ((llvm::StructType *)lostructTy->llvmType)->setBody(llvmmembers);

        auto typesize =
            typeMgr->getModule().getDataLayout().getTypeSizeInBits(
                lostructTy->llvmType) /
            8;

        lostructTy->complete(members, typesize);
    }
};

class TypeCollector : public AstVisitorAny {
    TypeTable *typeMgr;
    Scope *scope;

    std::list<AstStructDecl *> structDecls;
    std::list<AstFuncDecl *> funcDecls;

    using AstVisitorAny::visit;

    Object *visit(AstProgram *node) override {
        this->visit(node->body);
        return nullptr;
    }

    Object *visit(AstStatList *node) override {
        for (auto *it : node->body) {
            if (it->is<AstStructDecl>()) {
                structDecls.push_back(it->as<AstStructDecl>());
            } else if (it->is<AstFuncDecl>()) {
                funcDecls.push_back(it->as<AstFuncDecl>());
            }
        }
        return nullptr;
    }

    Object *visit(AstStructDecl *node) override {
        declareStructType(typeMgr, node);
        return nullptr;
    }

    Object *visit(AstFuncDecl *node) override {
        declareFunction(*scope, typeMgr, node);
        return nullptr;
    }

public:
    TypeCollector(TypeTable *typeMgr, Scope *scope, AstNode *root)
        : typeMgr(typeMgr), scope(scope) {
        this->visit(root);

        for (auto *it : structDecls) {
            this->visit(it);
        }

        for (auto *it : funcDecls) {
            this->visit(it);
        }

        for (auto *it : structDecls) {
            StructVisitor(typeMgr, it);
        }
    }
};

Function *
createFunc(Scope &scope, AstFuncDecl *root, StructType *parent) {
    initBuildinType(&scope);
    auto *func = declareFunction(scope, requireTypeTable(&scope), root);
    if (parent) {
        parent->addFunc(llvm::StringRef(root->name.tochara(), root->name.size()),
                        func);
    }
    return func;
}

void
scanningType(Scope *global, AstNode *root) {
    initBuildinType(global);
    TypeCollector(requireTypeTable(global), global, root);
}

StructType *
createStruct(Scope *scope, AstStructDecl *node) {
    initBuildinType(scope);
    auto *typeMgr = requireTypeTable(scope);
    auto *type = declareStructType(typeMgr, node);
    if (type->isOpaque()) {
        StructVisitor(typeMgr, node);
    }
    return type;
}

}  // namespace lona
