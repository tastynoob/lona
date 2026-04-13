#include "command.hh"
#include "tooling/output.hh"
#include "tooling/session.hh"
#include <algorithm>
#include <cctype>

namespace lona::tooling {

namespace {

bool
isKnownKindFilter(std::string_view word) {
    const auto normalized = trimCopy(word);
    return normalized == "all" || normalized == "type" ||
           normalized == "types" || normalized == "struct" ||
           normalized == "structs" || normalized == "trait" ||
           normalized == "traits" || normalized == "impl" ||
           normalized == "impls" || normalized == "func" ||
           normalized == "function" || normalized == "functions" ||
           normalized == "method" || normalized == "methods" ||
           normalized == "field" || normalized == "fields" ||
           normalized == "global" || normalized == "globals" ||
           normalized == "import" || normalized == "imports";
}

std::string
loadFailureMessage(const Session &session, std::string_view fallback) {
    if (!session.diagnostics().diagnostics().empty()) {
        return session.diagnostics().diagnostics().front().what();
    }
    return std::string(fallback);
}

bool
matchesCommand(std::string_view raw, const CommandSpec &spec,
               std::string &args) {
    const auto name = std::string_view(spec.name);
    if (raw == name) {
        args.clear();
        return true;
    }

    if (spec.argumentPolicy == CommandArgumentPolicy::None ||
        raw.size() <= name.size() || !raw.starts_with(name)) {
        return false;
    }

    if (!std::isspace(static_cast<unsigned char>(raw[name.size()]))) {
        return false;
    }

    args = trimCopy(raw.substr(name.size() + 1));
    return true;
}

CommandOutcome
handleQuit(Session &, const ParsedCommand &, OutputFormatter &,
           const CommandRegistry &) {
    return CommandOutcome{false, 0};
}

CommandOutcome
handleHelp(Session &, const ParsedCommand &command, OutputFormatter &formatter,
           const CommandRegistry &registry) {
    formatter.emitHelp(command.raw, registry);
    return {};
}

CommandOutcome
handleStatus(Session &session, const ParsedCommand &command,
             OutputFormatter &formatter, const CommandRegistry &) {
    formatter.emitStatus(command.raw, session);
    return {};
}

CommandOutcome
handleRoot(Session &session, const ParsedCommand &command,
           OutputFormatter &formatter, const CommandRegistry &) {
    if (command.args.empty()) {
        return formatter.emitError(command.raw, "root requires a path");
    }

    const auto rooted = session.setRootFile(command.args);
    if (!rooted) {
        return formatter.emitError(command.raw,
                                   loadFailureMessage(session, "root failed"));
    }
    formatter.emitLoadSummary(command.raw, session);
    return {};
}

CommandOutcome
handleReload(Session &session, const ParsedCommand &command,
             OutputFormatter &formatter, const CommandRegistry &) {
    if (command.args.empty()) {
        if (session.currentPath().empty()) {
            return formatter.emitError(command.raw,
                                       "reload requires a root source");
        }

        const auto reloaded = session.reload();
        if (!reloaded) {
            return formatter.emitError(
                command.raw, loadFailureMessage(session, "reload failed"));
        }
        formatter.emitLoadSummary(command.raw, session);
        return {};
    }

    if (!session.currentSourceIsFile() || session.currentPath().empty()) {
        return formatter.emitError(
            command.raw, "reload <path> requires a file-backed root project");
    }

    const auto reloaded = session.reloadFile(command.args);
    if (!reloaded) {
        return formatter.emitError(command.raw,
                                   loadFailureMessage(session, "reload failed"));
    }
    formatter.emitLoadSummary(command.raw, session);
    return {};
}

CommandOutcome
handleGoto(Session &session, const ParsedCommand &command,
           OutputFormatter &formatter, const CommandRegistry &) {
    auto line = parsePositiveInt(command.args);
    if (!line) {
        return formatter.emitError(command.raw,
                                   "goto requires a positive line number");
    }

    std::string error;
    if (!session.gotoLine(*line, &error)) {
        return formatter.emitError(command.raw, error);
    }
    formatter.emitCursor(command.raw, session);
    return {};
}

CommandOutcome
handleDiagnostics(Session &session, const ParsedCommand &command,
                  OutputFormatter &formatter, const CommandRegistry &) {
    formatter.emitDiagnostics(command.raw, session);
    return {};
}

CommandOutcome
handleInfoLocal(Session &session, const ParsedCommand &command,
                OutputFormatter &formatter, const CommandRegistry &) {
    int line = 0;
    if (!command.args.empty()) {
        auto parsed = parsePositiveInt(command.args);
        if (!parsed) {
            return formatter.emitError(
                command.raw, "info local requires a positive line number");
        }
        std::string error;
        if (!session.gotoLine(*parsed, &error)) {
            return formatter.emitError(command.raw, error);
        }
        line = *parsed;
    }

    formatter.emitInfoLocal(command.raw, session, line);
    return {};
}

CommandOutcome
handleAst(Session &session, const ParsedCommand &command,
          OutputFormatter &formatter, const CommandRegistry &) {
    formatter.emitAst(command.raw, session);
    return {};
}

CommandOutcome
handleInfoGlobal(Session &session, const ParsedCommand &command,
                 OutputFormatter &formatter, const CommandRegistry &) {
    formatter.emitInfoGlobal(command.raw, session);
    return {};
}

CommandOutcome
handlePrint(Session &session, const ParsedCommand &command,
            OutputFormatter &formatter, const CommandRegistry &) {
    if (command.args.empty()) {
        return formatter.emitError(command.raw, "print requires a symbol name");
    }
    return formatter.emitPrint(command.raw, session, command.args);
}

CommandOutcome
handleFind(Session &session, const ParsedCommand &command,
           OutputFormatter &formatter, const CommandRegistry &) {
    auto [kind, pattern] = parseFilterAndPattern(command.args);
    formatter.emitFind(command.raw, session, kind, pattern);
    return {};
}

}  // namespace

std::string
trimCopy(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(start, end - start));
}

std::optional<int>
parsePositiveInt(std::string_view text) {
    auto trimmed = trimCopy(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    std::size_t consumed = 0;
    int value = 0;
    try {
        value = std::stoi(trimmed, &consumed);
    } catch (...) {
        return std::nullopt;
    }
    if (consumed != trimmed.size() || value <= 0) {
        return std::nullopt;
    }
    return value;
}

std::pair<std::string, std::string>
parseFilterAndPattern(std::string_view text) {
    auto trimmed = trimCopy(text);
    if (trimmed.empty()) {
        return {"", ""};
    }

    const auto split = trimmed.find_first_of(" \t");
    if (split == std::string::npos) {
        if (isKnownKindFilter(trimmed)) {
            return {trimmed, ""};
        }
        return {"", trimmed};
    }

    auto first = trimCopy(trimmed.substr(0, split));
    auto rest = trimCopy(trimmed.substr(split + 1));
    if (isKnownKindFilter(first)) {
        return {first, rest};
    }
    return {"", trimmed};
}

void
CommandRegistry::add(CommandSpec spec) {
    indexByName_[spec.name] = commands_.size();
    commands_.push_back(std::move(spec));
}

const CommandSpec *
CommandRegistry::find(std::string_view name) const {
    auto found = indexByName_.find(std::string(name));
    if (found == indexByName_.end()) {
        return nullptr;
    }
    return &commands_[found->second];
}

std::optional<ParsedCommand>
CommandRegistry::parse(std::string_view raw) const {
    auto trimmed = trimCopy(raw);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    const CommandSpec *best = nullptr;
    std::string bestArgs;
    for (const auto &candidate : commands_) {
        std::string args;
        if (!matchesCommand(trimmed, candidate, args)) {
            continue;
        }
        if (!best || candidate.name.size() > best->name.size()) {
            best = &candidate;
            bestArgs = std::move(args);
        }
    }

    if (!best) {
        return std::nullopt;
    }

    return ParsedCommand{trimmed, best->name, std::move(bestArgs)};
}

CommandOutcome
CommandRegistry::dispatch(Session &session, std::string_view raw,
                          OutputFormatter &formatter) const { 
    auto parsed = parse(raw);
    if (!parsed) {
        auto trimmed = trimCopy(raw);
        if (trimmed.empty()) {
            return {};
        }
        return formatter.emitError(trimmed, "unknown command: " + trimmed);
    }

    auto *command = find(parsed->name);
    if (!command || !command->handler) {
        return formatter.emitError(parsed->raw,
                                   "internal error: command handler missing");
    }

    return command->handler(session, *parsed, formatter, *this);
}

std::vector<const CommandSpec *>
CommandRegistry::visibleCommands() const {
    std::vector<const CommandSpec *> visible;
    visible.reserve(commands_.size());
    for (const auto &command : commands_) {
        if (!command.hidden) {
            visible.push_back(&command);
        }
    }
    return visible;
}

CommandRegistry
buildCommandRegistry() {
    CommandRegistry registry;
    registry.add({"help", "help", "show this help",
                  CommandArgumentPolicy::None, false, handleHelp});
    registry.add({"status", "status", "show current session status",
                  CommandArgumentPolicy::None, false, handleStatus});
    registry.add({"root", "root <path>", "set the top-level module for this project",
                  CommandArgumentPolicy::Required, false, handleRoot});
    registry.add({"reload", "reload [path]",
                  "reload the whole project or one module and its dependents",
                  CommandArgumentPolicy::Optional, false, handleReload});
    registry.add({"goto", "goto <line>", "move the analysis point to a source line",
                  CommandArgumentPolicy::Required, false, handleGoto});
    registry.add({"info global", "info global", "print indexed non-local symbols",
                  CommandArgumentPolicy::None, false, handleInfoGlobal});
    registry.add({"diagnostics", "diagnostics", "print collected diagnostics",
                  CommandArgumentPolicy::None, false, handleDiagnostics});
    registry.add({"info local", "info local [line]",
                  "print locals visible at the current line",
                  CommandArgumentPolicy::Optional, false, handleInfoLocal});
    registry.add({"print", "print <name>",
                  "print one resolved symbol or field",
                  CommandArgumentPolicy::Required, false, handlePrint});
    registry.add({"ast", "ast", "print the parsed AST as JSON",
                  CommandArgumentPolicy::None, false, handleAst});
    registry.add({"find", "find [kind] [pattern]", "search indexed symbols",
                  CommandArgumentPolicy::Optional, false, handleFind});
    registry.add({"quit", "quit", "exit the session",
                  CommandArgumentPolicy::None, false, handleQuit});
    registry.add({"exit", "exit", "exit the session",
                  CommandArgumentPolicy::None, true, handleQuit});
    registry.add({"q", "q", "exit the session", CommandArgumentPolicy::None,
                  true, handleQuit});
    return registry;
}

}  // namespace lona::tooling
