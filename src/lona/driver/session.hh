#pragma once

#include "lona/driver/session_types.hh"
#include "lona/workspace/workspace_builder.hh"
#include "lona/workspace/workspace_loader.hh"
#include "lona/workspace/workspace.hh"
#include <iosfwd>
#include <string>

namespace lona {

class CompilerSession {
    CompilerWorkspace workspace_;
    WorkspaceLoader loader_;
    WorkspaceBuilder builder_;
    SessionStats lastStats_;

public:
    CompilerSession();
    ~CompilerSession();

    DiagnosticEngine &diagnostics() { return workspace_.diagnostics(); }
    const DiagnosticEngine &diagnostics() const { return workspace_.diagnostics(); }
    const SessionStats &lastStats() const { return lastStats_; }

    void printStats(std::ostream &out) const;
    int runEntry(const SessionOptions &options, std::ostream &out, std::ostream &diag);
    int runFile(const std::string &inputPath, const SessionOptions &options,
                std::ostream &out, std::ostream &diag);
};

}  // namespace lona
