#include "base.hh"
#include "type/buildin.hh"
#include "util/cfg.hh"
#include <fstream>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Pass.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#include <stack>
#include <unordered_map>

namespace lona {

llvm::PassBuilder passBuilder;
llvm::ModuleAnalysisManager moduleAM;
llvm::CGSCCAnalysisManager cgsccAM;
llvm::FunctionAnalysisManager functionAM;
llvm::LoopAnalysisManager loopAM;

// compiler for one file
class Compiler : public AstVisitor {
    llvm::LLVMContext &context;
    llvm::Module &module;
    llvm::IRBuilder<> builder;

    GlobalScope *globalScope = nullptr;
    FuncScope *headScope = nullptr;

    Object *struObj = nullptr;

public:
    Compiler(llvm::LLVMContext &context, llvm::Module &module)
        : context(context), module(module), builder(context) {
        // init buildin type
        globalScope = new GlobalScope(builder, module);
        initBuildinType(globalScope);

        // declare printf function
        std::vector<llvm::Type *> printfArgs;
        printfArgs.push_back(builder.getPtrTy());
        llvm::FunctionType *printfType =
            llvm::FunctionType::get(builder.getInt32Ty(), printfArgs, true);
        llvm::Function *func = llvm::Function::Create(
            printfType, llvm::Function::ExternalLinkage, "printf", module);
    }

    void printResult(std::ostream &os) {
        llvm::raw_os_ostream raw_os(os);

        module.print(raw_os, nullptr);
    }

    using AstVisitor::visit;

    Object *visit(AstProgram *node) override {
        // first scan the type
        scanningType(globalScope, node);

        // create main function
        llvm::FunctionType *funcType =
            llvm::FunctionType::get(builder.getInt32Ty(), false);
        llvm::Function *func =
            llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                   module.getName() + ".main", module);

        llvm::BasicBlock *entry =
            llvm::BasicBlock::Create(context, "entry", func);
        builder.SetInsertPoint(entry);
        headScope = new FuncScope(globalScope);
        node->body->accept(*this);

        builder.SetInsertPoint(&func->back());
        builder.CreateRet(builder.getInt32(0));
        return nullptr;
    }

    Object *visit(AstConst *node) override {
        if (node->getType() == AstConst::Type::STRING) {
            // create string
            auto lotype = headScope->getType("str");
            llvm::Constant *str = llvm::ConstantDataArray::getString(
                context, std::string(node->getBuf()));
            llvm::GlobalVariable *strval = new llvm::GlobalVariable(
                module, str->getType(), true, llvm::GlobalValue::PrivateLinkage,
                str);
            auto var =
                new BaseVar(strval, lotype, Object::REG_VAL | Object::READONLY);
            return var;
        } else if (node->getType() == AstConst::Type::FP32) {
            auto lotype = headScope->getType("f32");
            auto fpval =
                llvm::ConstantFP::get(lotype->llvmType, *node->getBuf<float>());
            return new BaseVar(fpval, lotype,
                               Object::REG_VAL | Object::READONLY);
        } else if (node->getType() == AstConst::Type::INT32) {
            auto intval =
                llvm::ConstantInt::get(i32Ty->llvmType, *node->getBuf<int>());
            return new BaseVar(intval, i32Ty,
                               Object::REG_VAL | Object::READONLY);
        } else {
            assert(false);
        }
        return nullptr;
    }

    Object *visit(AstField *node) override {
        // load value from local variable
        if (auto var = headScope->getObj(node->name)) {
            return var;
        } else {
            std::cout << node->loc << "Has no such variable: " << node->name
                      << std::endl;
            assert(false);
        }
        return nullptr;
    }

    Object *visit(AstAssign *node) override {
        // assign value to local variable
        auto dst = node->left->accept(*this);
        auto src = node->right->accept(*this);

        dst->set(builder, src);
        return nullptr;
    }

    Object *visit(AstBinOper *node) override {
        auto left = node->left->accept(*this);
        auto right = node->right->accept(*this);
        return left->getType()->binaryOperation(builder, left, node->op, right);
        ;
    }

    Object *visit(AstUnaryOper *node) override {}

    Object *visit(AstVarDecl *node) override {
        auto right = node->right ? node->right->accept(*this) : nullptr;

        try {
            Object *val = nullptr;

            if (node->typeHelper) {
                auto type = headScope->getType(node->typeHelper);
                if (!right || right->isRegVal()) {
                    val = headScope->allocate(type);
                } else {
                    // TODO: check type match
                    val = right;
                }
                headScope->addObj(node->field, val);
            } else if (node->right) {
                // auto infer
                if (right->isRegVal()) {
                    val = headScope->allocate(right->getType());
                    val->set(builder, right);
                } else {
                    val = right;
                }
                headScope->addObj(node->field, val);
            }

            return val;
        } catch (std::string &e) {
            std::cerr << node->loc << e << std::flush << std::endl;
            exit(-1);
        }
    }

    Object *visit(AstStatList *node) override {
        Object *final = nullptr;
        for (auto it = node->getBody().begin(); it != node->getBody().end();
             it++) {
            final = (*it)->accept(*this);
            if (headScope->isReturned()) break;
        }
        return final;
    }

    // ignore this
    Object *visit(AstStructDecl *node) override { return nullptr; }

    Object *visit(AstFuncDecl *node) override {
        // create function head
        Functional *func =
            dynamic_cast<Functional *>(headScope->getObj(node->name));
        auto llvmfunc = (llvm::Function *)nullptr;
        if (func) {
            llvmfunc = (llvm::Function *)func->get(builder);
        } else {
            func = createFunc(*headScope, node);
            llvmfunc = (llvm::Function *)func->get(builder);
            headScope->addObj(node->name, func);
        }

        // enter function scope
        auto upperScope = headScope;
        auto upperBB = builder.GetInsertBlock();
        headScope = new FuncScope(globalScope);
        auto *entry = llvm::BasicBlock::Create(context, "entry", llvmfunc);
        builder.SetInsertPoint(entry);

        // check control flow graph
        CFGChecker cfg;
        node->body->toCFG(cfg);
        std::ofstream os(headScope->getName() + ".dot");
        cfg.gen(os);
        os.close();

        if (cfg.hasNoReturnedNode() && node->retType) {
            std::cout << "Error: " << node->loc
                      << "No return statement in function " << node->name
                      << std::endl;
            exit(-1);
        }

        llvm::BasicBlock *retBB = nullptr;
        if (cfg.multiReturnPath()) {
            // create retBB when:
            // multi return path
            retBB = llvm::BasicBlock::Create(context);
            headScope->initRetBlock(retBB);
        }

        // make return alloca
        if (node->retType) {
            auto retType = headScope->getType(node->retType);
            headScope->initRetVal(headScope->allocate(retType));
        }
        // make args alloca
        for (int i = 0; i < llvmfunc->arg_size(); i++) {
            auto decl = dynamic_cast<AstVarDecl *>(node->args->at(i));
            auto lotype = headScope->getType(decl->typeHelper);
            auto llvmtype = lotype->llvmType;
            auto val = headScope->allocate(lotype);
            builder.CreateStore(llvmfunc->getArg(i), val->getllvmValue());
            headScope->addObj(decl->field, val);
        }
        // create function body
        scanningType(headScope, node->body);
        node->body->accept(*this);
        // set retbb to func's end
        // builder.CreateBr(retBB);
        if (retBB) {
            if (!headScope->isReturned()) builder.CreateBr(retBB);
            llvmfunc->insert(llvmfunc->end(), retBB);
            builder.SetInsertPoint(retBB);
        }
        
        if (!node->retType) {
            builder.CreateRetVoid();
        } else {
            // TODO:
            builder.CreateRet(headScope->retVal()->get(builder));
        }
        builder.SetInsertPoint(upperBB);
        delete headScope;
        headScope = upperScope;
        return nullptr;
    }

    Object *visit(AstRet *node) override {
        auto retalloc = headScope->retVal();
        if (retalloc && node->expr) {
            auto val = node->expr->accept(*this);
            retalloc->set(builder, val);
        } else if (retalloc && !node->expr) {
            throw "no return value";
        } else if (!retalloc && node->expr) {
            throw "no need return value";
        }

        headScope->setReturned();
        if (headScope->retBlock()) {
            builder.CreateBr(headScope->retBlock());
        }
        return retalloc;
    }

    Object *visit(AstIf *node) override {
        llvm::Function *func = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock *thenBB = llvm::BasicBlock::Create(context, "", func);
        llvm::BasicBlock *finalBB = llvm::BasicBlock::Create(context);
        llvm::BasicBlock *elseBB =
            node->hasElse() ? llvm::BasicBlock::Create(context) : finalBB;

        auto condval = node->condition->accept(*this);
        builder.CreateCondBr(condval->get(builder), thenBB, elseBB);
        Object *thenret = nullptr, *elsret = nullptr;

        auto lastScope = headScope;
        {
            builder.SetInsertPoint(thenBB);
            headScope = new LocalScope(lastScope);
            thenret = node->then->accept(*this);
            if (!headScope->isReturned()) builder.CreateBr(finalBB);
            delete headScope;
        }

        if (node->hasElse()) {
            func->insert(func->end(), elseBB);
            builder.SetInsertPoint(elseBB);
            headScope = new LocalScope(lastScope);
            elsret = node->els->accept(*this);
            if (!headScope->isReturned()) builder.CreateBr(finalBB);
            delete headScope;
        }
        headScope = lastScope;

        func->insert(func->end(), finalBB);
        builder.SetInsertPoint(finalBB);
        return nullptr;
    }

    Object *visit(AstFor *node) override {
        auto lastScope = headScope;

        llvm::Function *func = builder.GetInsertBlock()->getParent();
        auto condBB = llvm::BasicBlock::Create(context, "", func);
        auto loopBB = llvm::BasicBlock::Create(context, "", func);
        auto endBB = llvm::BasicBlock::Create(context);
        // create condBB
        builder.CreateBr(condBB);
        builder.SetInsertPoint(condBB);
        auto condval = node->expr->accept(*this);
        builder.CreateCondBr(condval->get(builder), loopBB, endBB);

        // create loop body
        headScope = new LocalScope(lastScope);
        builder.SetInsertPoint(loopBB);
        node->body->accept(*this);
        if (!headScope->isReturned()) builder.CreateBr(condBB);
        delete headScope;
        headScope = lastScope;

        // end
        func->insert(func->end(), endBB);
        builder.SetInsertPoint(endBB);
        return nullptr;
    }

    Object *visit(AstFieldCall *node) override {
        Functional *func = nullptr;
        std::vector<Object *> args;
        if (node->value->is<AstSelector>()) {
            auto parent =
                dynamic_cast<AstSelector *>(node->value)->parent->accept(*this);
            auto field = dynamic_cast<AstSelector *>(node->value)->field;
            auto member =
                parent->getType()->fieldSelect(builder, parent, field->text);
            func = dynamic_cast<Functional *>(member);
            // push self
            args.push_back(parent);
        } else {
            func = dynamic_cast<Functional *>(node->value->accept(*this));
        }

        if (node->args)
            for (auto it : *node->args) {
                auto arg = it->accept(*this);
                args.push_back(arg);
            }

        return func->getType()->callOperation(headScope, func, args);
    }

    Object *visit(AstSelector *node) override {
        auto parent = node->parent->accept(*this);
        auto field = node->field;
        return parent->getType()->fieldSelect(builder, parent, field->text);
    }
};

void
compile(AstNode *node, std::string &filename, std::ostream &os) {

    std::string module_name = filename.substr(0, filename.find_last_of('.'));
    llvm::LLVMContext context;
    llvm::Module module(module_name, context);
    // module.setDataLayout("e-m:e-i64:64-f80:128-n8:16:32:64-S128");

    Compiler compiler(context, module);
    compiler.visit(node);
    compiler.printResult(os);
}

}  // namespace lona