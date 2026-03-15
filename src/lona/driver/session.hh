#pragma once

#include "lona/ast/astnode.hh"
#include "lona/diag/diagnostic_engine.hh"
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

class Driver;

class CompilerSession {
    struct CompilationUnit;

    SourceManager sourceManager_;
    DiagnosticEngine diagnostics_;

public:
    CompilerSession();

    SourceManager &sourceManager() { return sourceManager_; }
    const SourceManager &sourceManager() const { return sourceManager_; }
    DiagnosticEngine &diagnostics() { return diagnostics_; }
    const DiagnosticEngine &diagnostics() const { return diagnostics_; }

    const SourceBuffer &loadSource(const std::string &path);
    AstNode *parseSource(const SourceBuffer &source);
    int emitJson(AstNode *tree, std::ostream &out) const;
    int emitIR(const SourceBuffer &source, AstNode *tree,
               const CompileOptions &options, std::ostream &out) const;
    int runFile(const std::string &inputPath, const SessionOptions &options,
                std::ostream &out, std::ostream &diag);
};

}  // namespace lona
