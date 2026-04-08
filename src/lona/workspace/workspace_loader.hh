#pragma once

#include "lona/module/compilation_unit.hh"
#include "workspace.hh"
#include <functional>
#include <string>
#include <vector>

namespace lona {

class WorkspaceLoader {
public:
    using ParseObserver =
        std::function<void(const CompilationUnit &, double, double)>;

private:
    CompilerWorkspace &workspace_;
    std::vector<std::string> includePaths_;

public:
    explicit WorkspaceLoader(CompilerWorkspace &workspace)
        : workspace_(workspace) {}

    void setIncludePaths(std::vector<std::string> includePaths);
    CompilationUnit &loadRootUnit(const std::string &path) const;
    AstNode *parseUnit(CompilationUnit &unit) const;
    void loadTransitiveUnits(ParseObserver observer = {}) const;
    void validateImportedUnit(const CompilationUnit &unit) const;

private:
    void discoverUnitDependencies(CompilationUnit &unit) const;
    std::vector<std::string> moduleRoots() const;
    std::vector<std::string> moduleRootsFor(const std::string &rootPath) const;
};

}  // namespace lona
