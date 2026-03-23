#pragma once

#include "lona/driver/session_types.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/sema/hir.hh"
#include "lona/type/scope.hh"
#include "lona/type/type.hh"
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace lona {
class ModuleGraph;

struct IRBuildState {
    llvm::LLVMContext context;
    llvm::Module module;
    llvm::IRBuilder<> builder;
    GlobalScope global;
    TypeTable types;

    IRBuildState(const CompilationUnit &unit, llvm::StringRef targetTriple);
};

struct IRPipelineContext {
    CompilationUnit &entryUnit;
    const ModuleGraph &moduleGraph;
    const CompilationUnit *rootUnit = nullptr;
    const CompileOptions &options;
    std::ostream &out;
    SessionStats &stats;
    IRBuildState build;
    HIRModule programHIR;
    std::vector<std::unique_ptr<HIRModule>> loweredModules;

    IRPipelineContext(CompilationUnit &entryUnit,
                      const ModuleGraph &moduleGraph,
                      const CompileOptions &options, std::ostream &out,
                      SessionStats &stats);
};

class CompilePipeline {
public:
    using StageFn = std::function<int(IRPipelineContext &)>;

private:
    struct Stage {
        std::string name;
        StageFn run;
    };

    std::vector<Stage> stages_;

public:
    void addStage(std::string name, StageFn run);
    int run(IRPipelineContext &context) const;
    std::vector<std::string> stageNames() const;
};

}  // namespace lona
