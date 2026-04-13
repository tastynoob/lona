#pragma once

#include "tooling/command.hh"
#include "tooling/session.hh"
#include <iosfwd>
#include <string_view>

namespace lona::tooling {

enum class OutputFormat {
    Text,
    Json,
};

class OutputFormatter {
    OutputFormat format_;
    std::ostream &out_;

    void emitJsonResponse(bool ok, std::string_view command, Json result) const;

public:
    OutputFormatter(OutputFormat format, std::ostream &out)
        : format_(format), out_(out) {}

    bool isJson() const { return format_ == OutputFormat::Json; }

    CommandOutcome emitError(std::string_view command, std::string message,
                             int exitCode = 1) const;
    void emitHelp(std::string_view command,
                  const CommandRegistry &registry) const;
    void emitStatus(std::string_view command, const Session &session) const;
    void emitLoadSummary(std::string_view command,
                         const Session &session) const;
    void emitCursor(std::string_view command, const Session &session) const;
    void emitDiagnostics(std::string_view command,
                         const Session &session) const;
    void emitInfoLocal(std::string_view command, const Session &session,
                       int line) const;
    void emitAst(std::string_view command, const Session &session) const;
    void emitInfoGlobal(std::string_view command,
                        const Session &session) const;
    void emitFind(std::string_view command, const Session &session,
                  std::string_view kindFilter,
                  std::string_view pattern) const;
    CommandOutcome emitPrint(std::string_view command, const Session &session,
                             std::string_view name) const;

    void printPrompt() const;
    void printRetryHint() const;
};

}  // namespace lona::tooling
