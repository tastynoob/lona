#pragma once

#include "lona/diag/diagnostic_bag.hh"
#include "lona/workspace/workspace.hh"
#include <cstddef>
#include <iosfwd>
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

class Session {
    CompilerWorkspace workspace_;
    std::string currentPath_;
    std::string currentSource_;
    bool currentSourceIsFile_ = false;
    bool sourceAvailable_ = false;
    int currentLine_ = 0;
    AstNode *syntaxTree_ = nullptr;
    DiagnosticBag diagnostics_;
    std::vector<SymbolRecord> symbols_;

    bool rebuild();
    void rebuildSymbolIndex();
    void tryCollectSemanticDiagnostics(CompilationUnit &unit);

public:
    explicit Session(std::size_t errorLimit = 20);

    bool openFile(const std::string &path);
    bool setSourceText(std::string path, std::string sourceText);
    bool reload();

    const std::string &currentPath() const { return currentPath_; }
    bool currentSourceIsFile() const { return currentSourceIsFile_; }
    bool hasLoadedSource() const { return sourceAvailable_; }
    int currentLine() const { return currentLine_; }
    bool hasTree() const { return syntaxTree_ != nullptr; }

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
    Json infoLocalJson(int line = 0) const;

    void printAst(std::ostream &out) const;
    void printDiagnostics(std::ostream &out) const;
    void printSymbols(std::ostream &out) const;
    void printFindResults(std::ostream &out, std::string_view kindFilter,
                          std::string_view pattern) const;
    void printInfoLocal(std::ostream &out, int line = 0) const;
};

}  // namespace lona::tooling
