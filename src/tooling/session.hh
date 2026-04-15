#pragma once

#include "lona/diag/diagnostic_bag.hh"
#include "lona/pass/compile_pipeline.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/hir.hh"
#include "lona/workspace/workspace.hh"
#include "lona/workspace/workspace_loader.hh"
#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lona::tooling {

struct SourceLocation {
    std::string path;
    int line = 0;
    int column = 0;
};

struct SymbolRecord {
    std::string kind;
    std::string name;
    std::string qualifiedName;
    std::string detail;
    SourceLocation loc;
};

struct AnalyzedFunctionRecord {
    const ResolvedFunction *resolved = nullptr;
    HIRFunc *hir = nullptr;
};

class Session {
    CompilerWorkspace workspace_;
    WorkspaceLoader loader_;
    std::string rootPath_;
    std::string currentPath_;
    std::vector<std::string> currentIncludePaths_;
    std::string currentSource_;
    bool currentSourceIsFile_ = false;
    bool sourceAvailable_ = false;
    int currentLine_ = 0;
    CompilationUnit *currentUnit_ = nullptr;
    AstNode *syntaxTree_ = nullptr;
    DiagnosticBag diagnostics_;
    std::vector<SymbolRecord> symbols_;
    std::unique_ptr<IRBuildState> analysisBuild_;
    std::unique_ptr<ResolvedModule> resolvedModule_;
    std::unique_ptr<HIRModule> analyzedModule_;
    std::vector<AnalyzedFunctionRecord> analyzedFunctions_;

    void resetQueryState();
    bool rebuildProject();
    bool rebuildProjectFromModule(const std::string &path);
    void rebuildSymbolIndex();
    void collectProjectSemanticDiagnostics();
    void rebuildActiveSemanticState(CompilationUnit &unit);
    void invalidateModuleAndDependents(const std::string &path);
    bool moduleBelongsToCurrentProject(const std::string &path) const;
    void finalizeActiveUnit(bool resetLine);
    bool activateFileModule(const std::string &path, bool resetLine,
                            std::string *errorMessage = nullptr);

public:
    explicit Session(std::size_t errorLimit = 20);

    bool setRootFile(const std::string &path,
                     std::vector<std::string> includePaths = {});
    bool setSourceText(std::string path, std::string sourceText);
    bool reload();
    bool reloadFile(const std::string &path);
    bool gotoModule(const std::string &path,
                    std::string *errorMessage = nullptr);

    const std::string &rootPath() const { return rootPath_; }
    const std::string &currentPath() const { return currentPath_; }
    const std::vector<std::string> &currentIncludePaths() const {
        return currentIncludePaths_;
    }
    bool currentSourceIsFile() const { return currentSourceIsFile_; }
    bool hasLoadedSource() const { return sourceAvailable_; }
    int currentLine() const { return currentLine_; }
    bool hasTree() const { return syntaxTree_ != nullptr; }
    bool hasResolvedModule() const { return resolvedModule_ != nullptr; }
    bool hasAnalysis() const { return analyzedModule_ != nullptr; }
    std::size_t analyzedFunctionCount() const { return analyzedFunctions_.size(); }

    const DiagnosticBag &diagnostics() const { return diagnostics_; }
    const std::vector<SymbolRecord> &symbols() const { return symbols_; }

    bool gotoLine(int line, std::string *errorMessage = nullptr);
    Json statusJson() const;
    Json cursorJson() const;
    Json astJson() const;
    Json diagnosticsJson() const;
    Json symbolsJson() const;
    Json findResultsJson(std::string_view kindFilter,
                         std::string_view pattern) const;
    Json fieldInfoJson(std::string_view fieldName) const;
    Json infoLocalJson(int line = 0) const;

    void printAst(std::ostream &out) const;
    void printDiagnostics(std::ostream &out) const;
    void printSymbols(std::ostream &out) const;
    void printFindResults(std::ostream &out, std::string_view kindFilter,
                          std::string_view pattern) const;
    void printFieldInfo(std::ostream &out, std::string_view fieldName) const;
    void printInfoLocal(std::ostream &out, int line = 0) const;
};

}  // namespace lona::tooling
