#pragma once


#include "../visitor.hh"
#include "lona/ast/astnode.hh"
#include "lona/obj/value.hh"
#include "lona/sema/hir.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include <cassert>
#include <cstddef>
#include <stack>
#include <vector>


namespace lona {

// semantics check
// HIR generation
class FuncAnalysis : public AstVisitor {

    TypeManager *typeMgr;
    Scope *scope;
    HIRFunc* hirfunc = new HIRFunc();

    std::vector<HIRNode*>* curBB;

    using AstVisitor::visit;

    ObjectPtr visit(AstStatList* node) override {
        for (auto it : node->getBody()) {
            this->visit(node);
        }
        return nullptr;
    }

    ObjectPtr visit(AstConst* node) override {

        auto type = node->getType();
        ObjectPtr val = nullptr;
        switch (type) {
            case AstConst::Type::INT32:
                val = new ConstVar(i32Ty, node->getBuf<int32_t>());
                break;
            default:
                assert(false);
        }

        scope->pushOp(val);
        return nullptr;
    }

    ObjectPtr visit(AstBinOper* node) override {
        this->visit(node->right);
        this->visit(node->left);

        auto robj = scope->popOp();
        auto lobj = scope->popOp();

        // check type
        if (robj->getType() != lobj->getType()) {
            throw std::runtime_error("Type mismatch in binary operation");
        }

        // check if the operation is valid
        // TODO

        // check if the operation is overloaded
        // TODO

        auto hir = new HIRBinOper(node->op,lobj,robj);

        curBB->push_back(hir);
        return nullptr;
    }

    ObjectPtr visit(AstVarDef* node) override {
        auto& var_name = node->getName();

        ObjectPtr initval = nullptr;
        if (auto right = node->getInitVal()) {
            this->visit(right);
            initval = scope->popOp();
        }

        if (auto typenode = node->getTypeNode()) {
            auto lotype = typeMgr->getType(typenode);
            // explict type definition
            // TODO
        }

        return nullptr;
    }



public:
    FuncAnalysis(TypeManager* typeMgr, GlobalScope* global, AstFuncDecl* node)
        : typeMgr(typeMgr), scope(new FuncScope(global)) {

        // deSROA
        auto lofunc = scope->getObj(node->name)->as<Function>();
        auto lofuncType = lofunc->getType()->as<FuncType>();

        /// push function params
        auto arg_index_begin = lofuncType->getArgTypes().begin();
        if (lofuncType->SROA()) {
            auto arg = arg_index_begin;
            hirfunc->pushArg((*arg)->newObj());
            arg_index_begin++;
        }
        int i=0;
        for (auto it = arg_index_begin; it != lofuncType->getArgTypes().end(); it++, i++) {
            auto obj = (*it)->newObj();
            auto arg_name = node->args->at(i)->as<AstVarDecl>()->field;
            hirfunc->pushArg(obj);
            scope->addObj(arg_name, obj);
        }
        
        curBB = &hirfunc->getBodyNodes();
        this->visit(node->body);


    }

};



}


