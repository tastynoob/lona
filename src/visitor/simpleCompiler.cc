#include "base.hh"
#include "type/typeclass.hh"
#include "type/variable.hh"
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

class Compiler : public AstVisitor {
    llvm::LLVMContext context = llvm::LLVMContext();
    llvm::IRBuilder<> builder = llvm::IRBuilder<>(context);
    llvm::Module module = llvm::Module("main", context);

    TypeManger *typeMgr = new TypeManger(builder);
    VariableManger *varMgr = new VariableManger(builder);

public:
    Compiler() {
        // declare printf function
        std::vector<llvm::Type *> printfArgs;
        printfArgs.push_back(builder.getPtrTy());
        llvm::FunctionType *printfType =
            llvm::FunctionType::get(builder.getInt32Ty(), printfArgs, true);
        llvm::Function *func = llvm::Function::Create(
            printfType, llvm::Function::ExternalLinkage, "printf", module);

        module.setDataLayout("e-m:e-i64:64-f80:128-n8:16:32:64-S128");
        module.setTargetTriple("x86_64-pc-linux-gnu");
        // module.setUwtable(llvm::UWTableKind::Async);
        // module.setPICLevel(llvm::PICLevel::BigPIC);
        // module.setPIELevel(llvm::PIELevel::Large);
        // module.setFramePointer(llvm::FramePointerKind::All);
        // module.setCodeModel(llvm::CodeModel::Large);
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

    BaseVariable *visit(AstProgram *node) override {
        // first scan the type
        scanningType(typeMgr, node);

        // create main function
        llvm::FunctionType *funcType =
            llvm::FunctionType::get(builder.getInt32Ty(), false);
        llvm::Function *func = llvm::Function::Create(
            funcType, llvm::Function::ExternalLinkage, "main", module);
        llvm::BasicBlock *entry =
            llvm::BasicBlock::Create(context, "entrypoint", func);
        builder.SetInsertPoint(entry);
        varMgr->enterScope();
        node->body->accept(*this);
        varMgr->leaveScope();

        builder.SetInsertPoint(&func->back());
        builder.CreateRet(builder.getInt32(0));
        return nullptr;
    }

    BaseVariable *visit(AstConst *node) override {
        if (node->getType() == AstConst::Type::STRING) {
            // create string
            auto lotype = typeMgr->getTypeClass("str");
            llvm::Constant *str = llvm::ConstantDataArray::getString(
                context, std::string(node->getBuf()));
            llvm::GlobalVariable *strval = new llvm::GlobalVariable(
                module, str->getType(), true, llvm::GlobalValue::PrivateLinkage,
                str);
            auto var = new BaseVariable(strval, lotype, BaseVariable::CONST);
            return var;
        } else if (node->getType() == AstConst::Type::FP32) {
            auto lotype = typeMgr->getTypeClass("f32");
            auto fpval = llvm::ConstantFP::get(lotype->getllvmType(),
                                               *node->getBuf<float>());
            return new BaseVariable(fpval, lotype, BaseVariable::CONST);
        } else if (node->getType() == AstConst::Type::INT32) {
            auto lotype = typeMgr->getTypeClass("i32");
            auto intval = llvm::ConstantInt::get(lotype->getllvmType(),
                                                 *node->getBuf<int>());
            return new BaseVariable(intval, lotype, BaseVariable::CONST);
        } else {
            assert(false);
        }
        return nullptr;
    }

    BaseVariable *visit(AstField *node) override {
        // load value from local variable
        if (auto var = varMgr->getVariable(node->name)) {
            return var;
        } else {
            assert(false);
        }
        return nullptr;
    }

    BaseVariable *visit(AstAssign *node) override {
        // assign value to local variable
        auto dst = node->left->accept(*this);
        auto src = node->right->accept(*this);

        dst->write(builder, src);
        return nullptr;
    }

    BaseVariable *visit(AstBinOper *node) override {
        auto left = node->left->accept(*this);
        auto right = node->right->accept(*this);
        return left->getType()->binaryOperation(builder, left, node->op, right);;
    }

    BaseVariable *visit(AstUnaryOper *node) override {}

    BaseVariable *visit(AstVarDecl *node) override {
        auto right = node->right ? node->right->accept(*this) : nullptr;
        BaseVariable *var = nullptr;
        if (node->typeHelper) {
            auto type = typeMgr->getTypeClass(node->typeHelper);
            auto val = builder.CreateAlloca(type->getllvmType());
            var = new BaseVariable(val, type);
            varMgr->addVariable(node->field, var);
        }
        if (node->right) {
            if (!var) {
                // auto infer
                auto alloc = builder.CreateAlloca(right->getType()->getllvmType());
                var = new BaseVariable(alloc, right->getType());
                varMgr->addVariable(node->field, var);
            }
            var->write(builder, right);
        }

        return var;
    }


    BaseVariable *visit(AstStatList *node) override {
        BaseVariable *final = nullptr;
        for (auto it = node->getBody().begin(); it != node->getBody().end();
             it++) {
            final = (*it)->accept(*this);
        }
        return final;
    }

    BaseVariable *visit(AstFuncDecl *node) override {}

    BaseVariable *visit(AstRet *node) override {}

    BaseVariable *visit(AstIf *node) override {
        llvm::Function *func = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock *thenBB = llvm::BasicBlock::Create(context, "", func);
        llvm::BasicBlock *finalBB = llvm::BasicBlock::Create(context);
        llvm::BasicBlock *elseBB =
            node->hasElse() ? llvm::BasicBlock::Create(context) : finalBB;

        auto condval = node->condition->accept(*this);
        BaseVariable *thenret, *elsret;
        builder.CreateCondBr(condval->read(builder), thenBB, elseBB);

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

    BaseVariable *visit(AstFieldCall *node) override {}
};

void
compile(AstNode *node, std::string &filename, std::ostream &os) {
    Compiler compiler;
    compiler.visit(node);
    compiler.printResult(os);
}

}  // namespace lona