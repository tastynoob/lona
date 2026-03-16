#pragma once

#include "lona/module/compilation_unit.hh"
#include "workspace.hh"
#include <functional>
#include <string>

namespace lona {

class WorkspaceLoader {
public:
    using ParseObserver = std::function<void(const CompilationUnit &, double)>;

private:
    CompilerWorkspace &workspace_;

public:
    explicit WorkspaceLoader(CompilerWorkspace &workspace)
        : workspace_(workspace) {}

    CompilationUnit &loadRootUnit(const std::string &path) const;
    AstNode *parseUnit(CompilationUnit &unit) const;
    void loadTransitiveUnits(ParseObserver observer = {}) const;
    void validateImportedUnit(const CompilationUnit &unit) const;

private:
    void discoverUnitDependencies(CompilationUnit &unit) const;
};

}  // namespace lona
