#pragma once

#include "lona/ast/astnode.hh"
#include "lona/sym/object.hh"
#include "lona/type/scope.hh"
#include "parser.hh"
#include <llvm-18/llvm/IR/DIBuilder.h>
#include <llvm-18/llvm/IR/DebugInfoMetadata.h>
#include <llvm-18/llvm/IR/IRBuilder.h>
#include <llvm-18/llvm/IR/Module.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lona {
namespace llvmcodegen_impl {

std::string
sourceFilename(const location &loc);

unsigned
sourceLine(const location &loc);

unsigned
sourceColumn(const location &loc);

struct DebugInfoContext {
    llvm::Module &module;
    TypeTable &typeTable;
    llvm::DIBuilder builder;
    llvm::DICompileUnit *compileUnit = nullptr;
    llvm::DIFile *primaryFile = nullptr;
    std::unordered_map<std::string, llvm::DIFile *> files;
    std::unordered_map<TypeClass *, llvm::DIType *> types;

    explicit DebugInfoContext(llvm::Module &module, TypeTable &types,
                              const std::string &sourcePath);

    llvm::DIFile *getOrCreateFile(const std::string &path);
    llvm::DIFile *fileForLocation(const location &loc);
    void finalize();
};

llvm::DIType *
getOrCreateDebugType(DebugInfoContext &debug, TypeClass *type);

llvm::DISubroutineType *
createDebugSubroutineType(DebugInfoContext &debug, FuncType *type);

llvm::DIScope *
debugScopeFor(DebugInfoContext &debug, llvm::Function *llvmFunc);

llvm::DISubprogram *
createDebugSubprogram(DebugInfoContext &debug, llvm::Function *llvmFunc,
                      FuncType *funcType, llvm::StringRef name,
                      const location &loc);

void
applyDebugLocation(llvm::IRBuilder<> &builder, DebugInfoContext *debug,
                   llvm::DIScope *scope, const location &loc);

void
clearDebugLocation(llvm::IRBuilder<> &builder);

void
emitDebugDeclare(DebugInfoContext *debug, FuncScope *scope,
                 llvm::DIScope *dbgScope, Object *obj, llvm::StringRef name,
                 TypeClass *type, const location &loc, unsigned argNo = 0);

}  // namespace llvmcodegen_impl
}  // namespace lona
