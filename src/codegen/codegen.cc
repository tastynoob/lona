#include "base.hh"
#include "type/buildin.hh"
#include "util/container.hh"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Pass.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#include <stack>
#include <unordered_map>

namespace lona {

// compiler for one file
class Compiler : public AstVisitor {
    llvm::LLVMContext &context;
    llvm::Module &module;
    llvm::IRBuilder<> builder;

    GlobalScope *globalScope = nullptr;
    Scope *headScope = nullptr;

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
        // llvm::legacy::PassManager pass;
        // pass.add(llvm::createPromoteMemoryToRegisterPass());  // mem2reg
        // pass.add(llvm::createCFGSimplificationPass());        // simplify cfg
        // pass.add(llvm::createDeadCodeEliminationPass());      // dead code
        //                                                       // elimination
        // pass.run(module);
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
            auto var = new RValue(strval, lotype, Object::READ_NO_LOAD);
            return var;
        } else if (node->getType() == AstConst::Type::FP32) {
            auto lotype = headScope->getType("f32");
            auto fpval = llvm::ConstantFP::get(lotype->getllvmType(),
                                               *node->getBuf<float>());
            return new RValue(fpval, lotype, Object::READ_NO_LOAD);
        } else if (node->getType() == AstConst::Type::INT32) {
            auto intval = llvm::ConstantInt::get(i32Ty->getllvmType(),
                                                 *node->getBuf<int>());
            return new RValue(intval, i32Ty, Object::READ_NO_LOAD);
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
            assert(false);
        }
        return nullptr;
    }

    Object *visit(AstAssign *node) override {
        // assign value to local variable
        auto dst = node->left->accept(*this);
        auto src = node->right->accept(*this);

        dst->write(builder, src);
        return nullptr;
    }

    Object *visit(AstBinOper *node) override {
        auto left = node->left->accept(*this);
        auto right = node->right->accept(*this);
        return left->getType()->binaryOperation(builder, left, node->op, right);
        ;
    }

    Object *visit(AstUnaryOper *node) override {}

    Object *visit(AstStructDecl *node) override {
        auto structTy = createStruct(headScope, node);
        headScope->addType(node->name, structTy);
        return nullptr;
    }

    Object *visit(AstVarDecl *node) override {
        auto right = node->right ? node->right->accept(*this) : nullptr;

        try {
            Object *val = nullptr;
            if (node->typeHelper) {
                auto type = headScope->getType(node->typeHelper);
                val = headScope->allocate(type);
                headScope->addObj(node->field, val);
            }

            if (node->right) {
                if (!val) {
                    // auto infer
                    val = headScope->allocate(right->getType());
                    headScope->addObj(node->field, val);
                }
                val->write(builder, right);
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
        }
        return final;
    }

    Object *visit(AstFuncDecl *node) override {
        // create function head
        Functional *func =
            dynamic_cast<Functional *>(headScope->getObj(node->name));
        auto llvmfunc = (llvm::Function *)nullptr;
        if (func) {
            llvmfunc = (llvm::Function *)func->read(builder);
        } else {
            func = node->createFunc(*headScope);
            llvmfunc = (llvm::Function *)func->read(builder);
            headScope->addObj(node->name, func);
        }

        // enter function scope
        auto upperScope = headScope;
        auto upperBB = builder.GetInsertBlock();
        headScope = new FuncScope(globalScope);
        llvm::BasicBlock *entry =
            llvm::BasicBlock::Create(context, "entry", llvmfunc);
        builder.SetInsertPoint(entry);
        // make args alloca
        for (int i = 0; i < llvmfunc->arg_size(); i++) {
            auto decl = dynamic_cast<AstVarDecl *>(node->args->at(i));
            auto lotype = headScope->getType(decl->typeHelper);
            auto llvmtype = lotype->getllvmType();
            auto val = headScope->allocate(lotype);
            builder.CreateStore(llvmfunc->getArg(i), val->getllvmValue());
            headScope->addObj(decl->field, val);
        }
        // create function body
        scanningType(headScope, node->body);
        node->body->accept(*this);
        if (!node->retType) {
            builder.CreateRetVoid();
        } else {
            // TODO:
            auto retBB = llvm::BasicBlock::Create(context, "", llvmfunc);
            builder.CreateBr(retBB);
            builder.SetInsertPoint(retBB);
        }
        builder.SetInsertPoint(upperBB);
        delete headScope;
        headScope = upperScope;
        return nullptr;
    }

    Object *visit(AstRet *node) override {}

    Object *visit(AstIf *node) override {
        llvm::Function *func = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock *thenBB = llvm::BasicBlock::Create(context, "", func);
        llvm::BasicBlock *finalBB = llvm::BasicBlock::Create(context);
        llvm::BasicBlock *elseBB =
            node->hasElse() ? llvm::BasicBlock::Create(context) : finalBB;

        auto condval = node->condition->accept(*this);
        builder.CreateCondBr(condval->read(builder), thenBB, elseBB);
        Object *thenret = nullptr, *elsret = nullptr;

        builder.SetInsertPoint(thenBB);
        thenret = node->then->accept(*this);
        builder.CreateBr(finalBB);

        if (node->hasElse()) {
            func->insert(func->end(), elseBB);
            builder.SetInsertPoint(elseBB);
            elsret = node->els->accept(*this);
            builder.CreateBr(finalBB);
        }

        func->insert(func->end(), finalBB);
        builder.SetInsertPoint(finalBB);
        return nullptr;
    }

    Object *visit(AstFieldCall *node) override {
        auto value = node->value->accept(*this);
        std::vector<Object *> args;
        if (node->args)
            for (auto it : *node->args) {
                auto arg = it->accept(*this);
                args.push_back(arg);
            }

        return value->getType()->callOperation(builder, value, args);
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
    module.setDataLayout("e-m:e-i64:64-f80:128-n8:16:32:64-S128");

    Compiler compiler(context, module);
    compiler.visit(node);
    compiler.printResult(os);
}

}  // namespace lona