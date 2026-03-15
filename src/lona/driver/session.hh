#pragma once

#include "lona/ast/astnode.hh"
#include "lona/diag/diagnostic_engine.hh"
#include "lona/module/module_graph.hh"
#include "lona/source/source_manager.hh"
#include <iosfwd>
#include <string>

namespace lona {

struct CompileOptions {
    int optLevel = 0;
    bool verifyIR = false;
    bool debugInfo = false;
};

enum class OutputMode {
    AstJson,
    LLVMIR,
};

struct SessionOptions {
    OutputMode outputMode = OutputMode::AstJson;
    CompileOptions compile;
};

struct SessionStats {
    std::size_t loadedUnits = 0;
    double parseMs = 0.0;
    double declarationMs = 0.0;
    double lowerMs = 0.0;
    double codegenMs = 0.0;
    double optimizeMs = 0.0;
    double verifyMs = 0.0;
    double totalMs = 0.0;
};

class Driver;

class CompilerSession {
    SourceManager sourceManager_;
    DiagnosticEngine diagnostics_;
    ModuleGraph moduleGraph_;
    SessionStats lastStats_;

    void discoverUnitDependencies(CompilationUnit &unit);
    void validateImportedUnit(const CompilationUnit &unit) const;

public:
    CompilerSession();

    SourceManager &sourceManager() { return sourceManager_; }
    const SourceManager &sourceManager() const { return sourceManager_; }
    DiagnosticEngine &diagnostics() { return diagnostics_; }
    const DiagnosticEngine &diagnostics() const { return diagnostics_; }
    ModuleGraph &moduleGraph() { return moduleGraph_; }
    const ModuleGraph &moduleGraph() const { return moduleGraph_; }
    const SessionStats &lastStats() const { return lastStats_; }

    const SourceBuffer &loadSource(const std::string &path);
    CompilationUnit &loadUnit(const std::string &path);
    CompilationUnit &loadRootUnit(const std::string &path);
    AstNode *parseUnit(CompilationUnit &unit);
    void parseLoadedUnits();
    void printStats(std::ostream &out) const;
    int emitJson(const CompilationUnit &unit, std::ostream &out) const;
    int emitIR(const CompilationUnit &unit,
               const CompileOptions &options, std::ostream &out);
    int runFile(const std::string &inputPath, const SessionOptions &options,
                std::ostream &out, std::ostream &diag);
};

}  // namespace lona
