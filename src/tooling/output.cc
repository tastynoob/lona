#include "output.hh"
#include <algorithm>
#include <iomanip>
#include <iostream>

namespace lona::tooling {

namespace {

constexpr std::string_view kTextPrompt = "lona-query> ";

Json
helpJson(const CommandRegistry &registry) {
    Json root = Json::object();
    root["commands"] = Json::array();
    for (const auto *command : registry.visibleCommands()) {
        root["commands"].push_back(command->usage);
    }
    root["formats"] = Json::array({"text", "json"});
    return root;
}

void
printHelp(std::ostream &out, const CommandRegistry &registry) {
    const auto commands = registry.visibleCommands();
    std::size_t width = 0;
    for (const auto *command : commands) {
        width = std::max(width, command->usage.size());
    }

    out << "commands:\n";
    for (const auto *command : commands) {
        out << "  " << std::left << std::setw(static_cast<int>(width))
            << command->usage << "  " << command->description << '\n';
    }
}

void
printStatus(std::ostream &out, const Session &session) {
    out << "root-paths: ";
    if (session.moduleRoots().empty()) {
        out << "<none>";
    } else {
        for (std::size_t i = 0; i < session.moduleRoots().size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << session.moduleRoots()[i];
        }
    }
    out << '\n';
    out << "entry-modules: ";
    if (session.loadedEntryPaths().empty()) {
        out << "<none>";
    } else {
        for (std::size_t i = 0; i < session.loadedEntryPaths().size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << session.loadedEntryPaths()[i];
        }
    }
    out << '\n';
    out << "module: "
        << (session.currentPath().empty() ? "<none>" : session.currentPath())
        << '\n';
    out << "source-kind: "
        << (session.currentPath().empty() && !session.currentSourceIsFile()
                ? "none"
                : (session.currentSourceIsFile() ? "file" : "memory"))
        << '\n';
    out << "syntax-tree: " << (session.hasTree() ? "yes" : "no") << '\n';
    out << "resolved: " << (session.hasResolvedModule() ? "yes" : "no")
        << '\n';
    out << "analysis: " << (session.hasAnalysis() ? "yes" : "no") << '\n';
    out << "cursor: ";
    if (session.currentLine() > 0) {
        out << session.currentLine();
    } else {
        out << "<none>";
    }
    out << '\n';
    out << "symbols: " << session.symbols().size() << '\n';
    out << "analyzed-functions: " << session.analyzedFunctionCount() << '\n';
    out << "diagnostics: " << session.diagnostics().size();
    if (session.diagnostics().truncated()) {
        out << " (truncated at " << session.diagnostics().maxErrors() << ')';
    }
    out << '\n';
}

void
printLoadSummary(std::ostream &out, const Session &session) {
    out << (session.hasLoadedSource() ? "loaded query state"
                                      : "updated root paths")
        << "; root-paths=" << session.moduleRoots().size()
        << "; entries=" << session.loadedEntryPaths().size() << "; active="
        << (session.currentPath().empty() ? "<none>" : session.currentPath())
        << "; syntax-tree=" << (session.hasTree() ? "yes" : "no")
        << "; resolved=" << (session.hasResolvedModule() ? "yes" : "no")
        << "; analysis=" << (session.hasAnalysis() ? "yes" : "no")
        << "; symbols=" << session.symbols().size()
        << "; analyzed-functions=" << session.analyzedFunctionCount()
        << "; diagnostics=" << session.diagnostics().size();
    if (session.diagnostics().truncated()) {
        out << " (truncated)";
    }
    out << '\n';
}

void
printCursor(std::ostream &out, const Session &session) {
    auto cursor = session.cursorJson();
    out << "cursor: " << cursor["path"].get<std::string>() << ':'
        << cursor["line"].get<int>();
    if (cursor["hasLocalScope"].get<bool>()) {
        out << " in " << cursor["context"]["kind"].get<std::string>() << ' '
            << cursor["context"]["name"].get<std::string>();
    } else {
        out << " (no local scope)";
    }
    out << '\n';
}

}  // namespace

void
OutputFormatter::emitJsonResponse(bool ok, std::string_view command,
                                  Json result) const {
    Json root = Json::object();
    root["ok"] = ok;
    root["command"] = std::string(command);
    root["result"] = std::move(result);
    out_ << root.dump() << '\n';
}

CommandOutcome
OutputFormatter::emitError(std::string_view command, std::string message,
                           int exitCode) const {
    if (isJson()) {
        Json result = Json::object();
        result["error"] = std::move(message);
        emitJsonResponse(false, command, std::move(result));
    } else {
        out_ << message << '\n';
    }
    return CommandOutcome{true, exitCode};
}

void
OutputFormatter::emitHelp(std::string_view command,
                          const CommandRegistry &registry) const {
    if (isJson()) {
        emitJsonResponse(true, command, helpJson(registry));
    } else {
        printHelp(out_, registry);
    }
}

void
OutputFormatter::emitStatus(std::string_view command,
                            const Session &session) const {
    if (isJson()) {
        emitJsonResponse(true, command, session.statusJson());
    } else {
        printStatus(out_, session);
    }
}

void
OutputFormatter::emitLoadSummary(std::string_view command,
                                 const Session &session) const {
    if (isJson()) {
        emitJsonResponse(true, command, session.statusJson());
    } else {
        printLoadSummary(out_, session);
    }
}

void
OutputFormatter::emitCursor(std::string_view command,
                            const Session &session) const {
    if (isJson()) {
        emitJsonResponse(true, command, session.cursorJson());
    } else {
        printCursor(out_, session);
    }
}

void
OutputFormatter::emitDiagnostics(std::string_view command,
                                 const Session &session) const {
    if (isJson()) {
        emitJsonResponse(true, command, session.diagnosticsJson());
    } else {
        session.printDiagnostics(out_);
    }
}

void
OutputFormatter::emitInfoLocal(std::string_view command,
                               const Session &session, int line) const {
    if (isJson()) {
        emitJsonResponse(true, command, session.infoLocalJson(line));
    } else {
        session.printInfoLocal(out_, line);
    }
}

void
OutputFormatter::emitAst(std::string_view command, const Session &session) const {
    if (isJson()) {
        emitJsonResponse(true, command, session.astJson());
    } else {
        session.printAst(out_);
    }
}

void
OutputFormatter::emitInfoGlobal(std::string_view command,
                                const Session &session) const {
    if (isJson()) {
        emitJsonResponse(true, command, session.symbolsJson());
    } else {
        session.printSymbols(out_);
    }
}

void
OutputFormatter::emitFind(std::string_view command, const Session &session,
                          std::string_view kindFilter,
                          std::string_view pattern) const {
    if (isJson()) {
        emitJsonResponse(true, command,
                         session.findResultsJson(kindFilter, pattern));
    } else {
        session.printFindResults(out_, kindFilter, pattern);
    }
}

CommandOutcome
OutputFormatter::emitPrint(std::string_view command, const Session &session,
                           std::string_view name,
                           PrintQueryKind kind) const {
    auto result = session.printItemJson(name, kind);
    if (!result["found"].get<bool>()) {
        if (isJson()) {
            emitJsonResponse(false, command, std::move(result));
        } else {
            session.printItem(out_, name, kind);
        }
        return CommandOutcome{true, 1};
    }

    if (isJson()) {
        emitJsonResponse(true, command, std::move(result));
    } else {
        session.printItem(out_, name, kind);
    }
    return {};
}

std::string_view
OutputFormatter::promptText() const {
    return isJson() ? std::string_view{} : kTextPrompt;
}

void
OutputFormatter::printPrompt() const {
    if (!isJson()) {
        out_ << promptText() << std::flush;
    }
}

void
OutputFormatter::printRetryHint() const {
    if (!isJson()) {
        out_ << "type `help` for available commands\n";
    }
}

}  // namespace lona::tooling
