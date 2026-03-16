#pragma once

#include "lona/driver/session_types.hh"
#include "lona/module/module_executor.hh"
#include "lona/pass/compile_pipeline.hh"
#include "workspace_loader.hh"
#include "workspace.hh"
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>

namespace lona {

class WorkspaceBuilder {
    CompilerWorkspace &workspace_;
    const WorkspaceLoader &loader_;
    std::unique_ptr<ModuleExecutor> executor_;
    CompilePipeline pipeline_;

    std::unordered_map<std::string, std::uint64_t>
    collectDependencyInterfaceHashes(const CompilationUnit &unit) const;
    bool matchesArtifact(const CompilationUnit &unit,
                         const ModuleArtifact &artifact) const;
    const ModuleArtifact *reusableArtifactFor(const CompilationUnit &unit) const;
    ModuleArtifact createArtifact(const CompilationUnit &unit) const;
    int compileModule(CompilationUnit &unit, const CompileOptions &options,
                      ModuleArtifact &artifact, SessionStats &stats,
                      std::ostream &out) const;
    int linkArtifacts(const CompilationUnit &rootUnit, bool verifyIR,
                      std::ostream &out, double *linkMs = nullptr,
                      double *verifyMs = nullptr) const;

public:
    WorkspaceBuilder(CompilerWorkspace &workspace, const WorkspaceLoader &loader);

    std::size_t loadedUnitCount() const;
    int emitIR(CompilationUnit &rootUnit, const CompileOptions &options,
               SessionStats &stats, std::ostream &out) const;
};

}  // namespace lona
