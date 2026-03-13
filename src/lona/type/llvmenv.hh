#pragma once


#include <llvm-18/llvm/IR/IRBuilder.h>
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/Type.h>


namespace lona {

class Env : public llvm::Module {
public:
    Env(const std::string& module_name, llvm::LLVMContext& context)
        : llvm::Module(module_name, context) {}

    ~Env() = default;
};

}


