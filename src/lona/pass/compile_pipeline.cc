#include "compile_pipeline.hh"
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <utility>

namespace lona {

IRBuildState::IRBuildState(const CompilationUnit &unit)
    : module(unit.path(), context),
      builder(context),
      global(builder, module),
      types(module) {
    global.setTypeTable(&types);
}

IRPipelineContext::IRPipelineContext(const CompilationUnit &entryUnit,
                                     const ModuleGraph &moduleGraph,
                                     const CompileOptions &options,
                                     std::ostream &out, SessionStats &stats)
    : entryUnit(entryUnit),
      moduleGraph(moduleGraph),
      options(options),
      out(out),
      stats(stats),
      build(entryUnit) {}

void
CompilePipeline::addStage(std::string name, StageFn run) {
    stages_.push_back({std::move(name), std::move(run)});
}

int
CompilePipeline::run(IRPipelineContext &context) const {
    for (const auto &stage : stages_) {
        int exitCode = stage.run(context);
        if (exitCode != 0) {
            return exitCode;
        }
    }
    return 0;
}

std::vector<std::string>
CompilePipeline::stageNames() const {
    std::vector<std::string> names;
    names.reserve(stages_.size());
    for (const auto &stage : stages_) {
        names.push_back(stage.name);
    }
    return names;
}

}  // namespace lona
