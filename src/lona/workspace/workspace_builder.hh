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
    enum class BundleArtifactKind {
        Bitcode,
        Object,
    };

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
    static ModuleEntryRole artifactEntryRoleFor(
        const CompilationUnit &unit, const CompilationUnit &rootUnit);
    std::string bundleMemberFileName(const ModuleArtifact &artifact,
                                     BundleArtifactKind kind) const;
    std::filesystem::path bundleMemberPath(
        const ModuleArtifact &artifact,
        const std::filesystem::path &bundleDir,
        BundleArtifactKind kind) const;
    void persistArtifactOutput(const ModuleArtifact &artifact,
                               const std::filesystem::path &artifactCacheDir,
                               BundleArtifactKind kind) const;
    bool matchesArtifact(const CompilationUnit &unit,
                         const ModuleArtifact &artifact,
                         const CompileOptions &options,
                         ModuleEntryRole entryRole,
                         const GenericInstanceRegistry &instanceRegistry) const;
    ModuleArtifact *reusableArtifactFor(const CompilationUnit &unit,
                                        const CompileOptions &options,
                                        const CompilationUnit &rootUnit,
                                        const GenericInstanceRegistry &instanceRegistry) const;
    ModuleArtifact createArtifact(const CompilationUnit &unit,
                                  const CompileOptions &options,
                                  const CompilationUnit &rootUnit) const;
    int ensureArtifactOutputs(ModuleArtifact &artifact,
                              const CompileOptions &options,
                              bool requireObjects, bool requireBitcode,
                              SessionStats &stats) const;
    int buildArtifacts(CompilationUnit &rootUnit, const CompileOptions &options,
                       bool requireObjects, bool requireBitcode,
                       const std::filesystem::path *artifactCacheDir,
                       SessionStats &stats, std::ostream &out) const;
    int compileModule(CompilationUnit &unit, const CompileOptions &options,
                      ModuleArtifact &artifact, bool emitObject,
                      bool emitBitcode,
                      GenericInstanceRegistry &instanceRegistry,
                      SessionStats &stats,
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
    int emitBitcodeBundle(CompilationUnit &rootUnit,
                          const CompileOptions &options,
                          const std::string &outputPath,
                          const std::string &cacheOutputPath,
                          SessionStats &stats, std::ostream &out) const;
    int emitObjectBundle(CompilationUnit &rootUnit,
                         const CompileOptions &options,
                         const std::string &outputPath,
                         const std::string &cacheOutputPath,
                         SessionStats &stats, std::ostream &out) const;
    int emitLinkedObject(CompilationUnit &rootUnit,
                         const CompileOptions &options,
                         const std::string &outputPath,
                         const std::string &artifactCachePath,
                         SessionStats &stats, std::ostream &out) const;
};

}  // namespace lona
