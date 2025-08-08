#include "../visitor.hh"
#include "../type/scope.hh"
#include "lona/ast/astnode.hh"
#include "lona/obj/value.hh"
#include "lona/type/type.hh"
#include <cassert>
#include <cstddef>
#include <list>
#include <llvm-18/llvm/IR/DerivedTypes.h>
#include <llvm-18/llvm/IR/Type.h>
#include <string>
#include <vector>

namespace lona {

class StructVisitor : public AstVisitorAny {
    TypeManager* typeMgr;

    std::vector<llvm::Type*> llvmmembers;
    llvm::StringMap<StructType::ValueTy> members;
    llvm::StringMap<Function*> funcs;

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
        auto type = typeMgr->getType(node->typeNode);

        llvmmembers.push_back(type->llvmType);
        members.insert({name, {type, llvmmembers.size() - 1}});

        return nullptr;
    }

public:
    StructVisitor(TypeManager* typeMgr, AstStructDecl* node)
        : typeMgr(typeMgr) {

        auto type = typeMgr->getType(node->name);
        assert(type);
        auto lostructTy = type->as<StructType>();
        assert(lostructTy);

        this->visit(node->body);

        ((llvm::StructType*)lostructTy->llvmType)->setBody(llvmmembers);

        auto typesize = typeMgr->getModule().getDataLayout().getTypeSizeInBits(
                            lostructTy->llvmType) / 8;

        lostructTy->complete(members, typesize);

        for (auto& it : funcs) {
            lostructTy->addFunc(it.first().str(), it.second);
        }
    }
};


class TypeCollector : public AstVisitorAny {
    TypeManager* typeMgr;
    Scope* scope;

    std::list<AstStructDecl*> structDecls;
    std::list<AstFuncDecl*> funcDecls;

    using AstVisitor::visit;
    Object *visit(AstProgram* node) override {
        this->visit(node->body);
        return nullptr;
    }

    Object *visit(AstStatList* node) override {
        for (auto it : node->body) {
            if (it->is<AstStructDecl>()) {
                structDecls.push_back(it->as<AstStructDecl>());
            } else if (it->is<AstFuncDecl>()) {
                funcDecls.push_back(it->as<AstFuncDecl>());
            }
        }
        return nullptr;
    };

    Object* visit(AstStructDecl* node) override {
        // collect struct type

        // the struct type name should not be modify
        auto struct_name = node->name;

        if (typeMgr->getType(struct_name)) {
            // error
            assert(false);
        }

        auto structTy = llvm::StructType::create(
            typeMgr->getContext(), struct_name);
        auto lostructTy = new StructType(structTy, struct_name);

        typeMgr->addType(struct_name, lostructTy);
        return nullptr;
    }

    Object *visit(AstFuncDecl* node) override {
        // create function type
        auto& func_name = node->name;

        std::vector<TypeClass*> loargs;
        std::vector<llvm::Type*> args;

        TypeClass* loretType = nullptr;
        llvm::Type* retType = llvm::Type::getVoidTy(typeMgr->getContext());
        

        // the function type name should be:
        // f + "_" + retType + "." + args[0] + "." + args[1] + ...
        // or f + "." + args[0] + "." + args[1] + ...
        std::string funcType_name = "f";
        if (node->retType) {
            loretType = typeMgr->getType(node->retType);
            retType = loretType->llvmType;
            funcType_name += "_" + loretType->full_name;

            if (loretType->shouldReturnByPointer()) {
                auto sroa_retType = typeMgr->createPointerType(loretType);
                loargs.push_back(sroa_retType);
                args.push_back(sroa_retType->llvmType);
            }
        }

        if (node->args) {
            for (auto arg: *node->args) {
                assert(arg->is<AstVarDecl>());
                auto varDecl = arg->as<AstVarDecl>();
                auto type = typeMgr->getType(varDecl->typeNode);
                loargs.push_back(type);
                args.push_back(type->llvmType);
                funcType_name += "." + type->full_name;
            }
        }

        TypeClass* lofuncType = typeMgr->getType(funcType_name);
        if (!lofuncType) {
            // create new function type
            auto llfuncType= llvm::FunctionType::get(retType, args, false);
            lofuncType = new FuncType(
                llfuncType, std::move(loargs), loretType, func_name,
                typeMgr->getModule().getDataLayout().getTypeSizeInBits(llfuncType) / 8);
            typeMgr->addType(funcType_name, lofuncType);
        }

        if (scope->getObj(func_name)) {
            // error
            assert(false);
        }

        // create function instance
        auto llfunc = llvm::Function::Create(
            (llvm::FunctionType*)lofuncType->llvmType,
            llvm::Function::ExternalLinkage, func_name,
            typeMgr->getModule());
        auto lofunc = new Function(llfunc, lofuncType->as<FuncType>());

        scope->addObj(func_name, lofunc);
        return nullptr;
    }

public:
    TypeCollector(TypeManager* typeMgr, Scope* scope, AstProgram* program) : typeMgr(typeMgr), scope(scope) {
        this->visit(program);
        // collect all struct and function decls
        for (auto it : structDecls) {
            this->visit(it);
        }

        for (auto it : funcDecls) {
            this->visit(it);
        }

        // complete all struct types
        for (auto it : structDecls) {
            auto t = StructVisitor(typeMgr, it);
        }
    }
};

    

}