#pragma once

#include "lona/diag/diagnostic_bag.hh"
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
    DiagnosticBag *diagnostics_ = nullptr;

public:
    explicit WorkspaceLoader(CompilerWorkspace &workspace)
        : workspace_(workspace) {}

    void setIncludePaths(std::vector<std::string> includePaths);
    void setDiagnosticBag(DiagnosticBag *diagnostics) {
        diagnostics_ = diagnostics;
    }
    CompilationUnit &loadRootUnit(const std::string &path) const;
    std::string resolveModuleFilePath(const std::string &rootPath,
                                      const std::string &modulePath) const;
    AstNode *parseUnit(CompilationUnit &unit) const;
    void loadTransitiveUnitsFrom(const std::string &path,
                                 ParseObserver observer = {}) const;
    void loadTransitiveUnits(ParseObserver observer = {}) const;
    void validateImportedUnit(const CompilationUnit &unit) const;

private:
    void discoverUnitDependencies(CompilationUnit &unit) const;
    std::vector<std::string> moduleRoots() const;
    std::vector<std::string> moduleRootsFor(const std::string &rootPath) const;
};

}  // namespace lona
