#pragma once

#include "lona/driver/session_types.hh"
#include "lona/module/module_executor.hh"
#include "lona/pass/compile_pipeline.hh"
#include "workspace.hh"
#include "workspace_loader.hh"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <llvm-18/llvm/IR/Module.h>
#include <memory>
#include <string>
#include <unordered_map>

namespace lona {

class WorkspaceBuilder {
    struct LinkedModule {
        std::unique_ptr<llvm::LLVMContext> context;
        std::unique_ptr<llvm::Module> module;
    };

    CompilerWorkspace &workspace_;
    const WorkspaceLoader &loader_;
    std::unique_ptr<ModuleExecutor> executor_;
    CompilePipeline pipeline_;

    std::unordered_map<string, std::uint64_t> collectDependencyInterfaceHashes(
        const CompilationUnit &unit) const;
    std::string bundleObjectFileName(const ModuleArtifact &artifact) const;
    std::filesystem::path bundleObjectPath(
        const ModuleArtifact &artifact,
        const std::filesystem::path &bundleDir) const;
    bool matchesArtifact(const CompilationUnit &unit,
                         const ModuleArtifact &artifact,
                         const CompileOptions &options) const;
    ModuleArtifact *reusableArtifactFor(const CompilationUnit &unit,
                                        const CompileOptions &options) const;
    ModuleArtifact createArtifact(const CompilationUnit &unit,
                                  const CompileOptions &options) const;
    int ensureArtifactOutputs(ModuleArtifact &artifact,
                              const CompileOptions &options,
                              bool requireObjects, bool requireBitcode,
                              SessionStats &stats) const;
    int buildArtifacts(CompilationUnit &rootUnit, const CompileOptions &options,
                       bool requireObjects, bool requireBitcode,
                       const std::filesystem::path *objectCacheDir,
                       SessionStats &stats, std::ostream &out) const;
    int compileModule(CompilationUnit &unit, const CompileOptions &options,
                      ModuleArtifact &artifact, bool emitObject,
                      bool emitBitcode, SessionStats &stats,
                      std::ostream &out) const;
    LinkedModule linkArtifacts(const CompilationUnit &rootUnit,
                               bool hostedEntry, bool verifyIR,
                               SessionStats &stats, std::ostream &out) const;

public:
    WorkspaceBuilder(CompilerWorkspace &workspace,
                     const WorkspaceLoader &loader);

    std::size_t loadedUnitCount() const;
    int emitHostedEntryObject(const CompileOptions &options,
                              const std::string &outputPath,
                              SessionStats &stats, std::ostream &out) const;
    int emitIR(CompilationUnit &rootUnit, const CompileOptions &options,
               SessionStats &stats, std::ostream &out) const;
    int emitObject(CompilationUnit &rootUnit, const CompileOptions &options,
                   const std::string &outputPath, SessionStats &stats,
                   std::ostream &out) const;
    int emitObjectBundle(CompilationUnit &rootUnit,
                         const CompileOptions &options,
                         const std::string &outputPath,
                         const std::string &cacheOutputPath,
                         SessionStats &stats, std::ostream &out) const;
};

}  // namespace lona
