#pragma once

#include "lona/diag/diagnostic_engine.hh"
#include "lona/module/build_queue.hh"
#include "lona/module/module_artifact.hh"
#include "lona/module/module_cache.hh"
#include "lona/module/module_graph.hh"
#include "lona/source/source_manager.hh"
#include <string>
#include <unordered_map>

namespace lona {

class CompilerWorkspace {
    SourceManager sourceManager_;
    DiagnosticEngine diagnostics_;
    ModuleCache moduleCache_;
    ModuleGraph moduleGraph_;
    ModuleBuildQueue buildQueue_;
    std::unordered_map<string, ModuleArtifact> moduleArtifacts_;

public:
    CompilerWorkspace();

    SourceManager &sourceManager() { return sourceManager_; }
    const SourceManager &sourceManager() const { return sourceManager_; }

    DiagnosticEngine &diagnostics() { return diagnostics_; }
    const DiagnosticEngine &diagnostics() const { return diagnostics_; }

    ModuleCache &moduleCache() { return moduleCache_; }
    const ModuleCache &moduleCache() const { return moduleCache_; }

    ModuleGraph &moduleGraph() { return moduleGraph_; }
    const ModuleGraph &moduleGraph() const { return moduleGraph_; }

    ModuleBuildQueue &buildQueue() { return buildQueue_; }
    const ModuleBuildQueue &buildQueue() const { return buildQueue_; }

    std::unordered_map<string, ModuleArtifact> &moduleArtifacts() {
        return moduleArtifacts_;
    }
    const std::unordered_map<string, ModuleArtifact> &moduleArtifacts() const {
        return moduleArtifacts_;
    }

    const SourceBuffer &loadSource(const string &path);
    const SourceBuffer &loadSource(const std::string &path) {
        return loadSource(string(path));
    }
    CompilationUnit &loadUnit(const string &path);
    CompilationUnit &loadUnit(const std::string &path) {
        return loadUnit(string(path));
    }
    CompilationUnit &loadRootUnit(const string &path);
    CompilationUnit &loadRootUnit(const std::string &path) {
        return loadRootUnit(string(path));
    }

    ModuleArtifact *findArtifact(const string &path);
    ModuleArtifact *findArtifact(const std::string &path) {
        return findArtifact(string(path));
    }
    const ModuleArtifact *findArtifact(const string &path) const;
    const ModuleArtifact *findArtifact(const std::string &path) const {
        return findArtifact(string(path));
    }
    void storeArtifact(ModuleArtifact artifact);
};

}  // namespace lona
