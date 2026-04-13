#include "cmdline.hpp"
#include "tooling/session.hh"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

enum class OutputFormat {
    Text,
    Json,
};

struct CommandOutcome {
    bool keepRunning = true;
    int exitCode = 0;
};

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

Json
helpJson() {
    Json root = Json::object();
    root["commands"] = Json::array({
        "help",
        "status",
        "open <path>",
        "reload",
        "goto <line>",
        "info global",
        "diagnostics",
        "print diagnostics",
        "info local [line]",
        "ast",
        "print ast",
        "find [kind] [pattern]",
        "quit",
    });
    root["formats"] = Json::array({"text", "json"});
    return root;
}

void
emitJsonResponse(std::ostream &out, bool ok, std::string_view command,
                 Json result) {
    Json root = Json::object();
    root["ok"] = ok;
    root["command"] = std::string(command);
    root["result"] = std::move(result);
    out << root.dump() << '\n';
}

CommandOutcome
emitCommandError(OutputFormat format, std::ostream &out,
                 std::string_view command, std::string message,
                 int exitCode = 1) {
    if (format == OutputFormat::Json) {
        Json result = Json::object();
        result["error"] = std::move(message);
        emitJsonResponse(out, false, command, std::move(result));
    } else {
        out << message << '\n';
    }
    return CommandOutcome{true, exitCode};
}

std::string
loadFailureMessage(const lona::tooling::Session &session,
                   std::string_view fallback) {
    if (!session.diagnostics().diagnostics().empty()) {
        return session.diagnostics().diagnostics().front().what();
    }
    return std::string(fallback);
}

void
printHelp(std::ostream &out) {
    out << "commands:\n"
        << "  help                      show this help\n"
        << "  status                    show current session status\n"
        << "  open <path>               load source from file\n"
        << "  reload                    reload the current source\n"
        << "  goto <line>               move the analysis point to a source line\n"
        << "  info global               print indexed non-local symbols\n"
        << "  diagnostics               print collected diagnostics\n"
        << "  info local [line]         print locals visible at the current line;\n"
        << "                            with a line argument it also moves there\n"
        << "  print ast                 print the parsed AST as JSON\n"
        << "  find [kind] [pattern]     search symbols; kind can be type, struct,\n"
        << "                            trait, impl, func, method, field, global,\n"
        << "                            import, or all\n"
        << "  ast                       alias of `print ast`\n"
        << "  symbols                   compatibility alias of `info global`\n"
        << "  print symbols             compatibility alias of `info global`\n"
        << "  quit                      exit the session\n";
}

void
printStatus(const lona::tooling::Session &session, std::ostream &out) {
    out << "path: "
        << (session.currentPath().empty() ? "<none>" : session.currentPath())
        << '\n';
    out << "source-kind: "
        << (session.currentPath().empty()
                ? "none"
                : (session.currentSourceIsFile() ? "file" : "memory"))
        << '\n';
    out << "syntax-tree: " << (session.hasTree() ? "yes" : "no") << '\n';
    out << "cursor: ";
    if (session.currentLine() > 0) {
        out << session.currentLine();
    } else {
        out << "<none>";
    }
    out << '\n';
    out << "symbols: " << session.symbols().size() << '\n';
    out << "diagnostics: " << session.diagnostics().size();
    if (session.diagnostics().truncated()) {
        out << " (truncated at " << session.diagnostics().maxErrors() << ')';
    }
    out << '\n';
}

void
printLoadSummary(const lona::tooling::Session &session, std::ostream &out) {
    out << (session.hasLoadedSource() ? "loaded " : "unavailable ")
        << (session.currentPath().empty() ? "<none>" : session.currentPath())
        << "; syntax-tree=" << (session.hasTree() ? "yes" : "no")
        << "; symbols=" << session.symbols().size()
        << "; diagnostics=" << session.diagnostics().size();
    if (session.diagnostics().truncated()) {
        out << " (truncated)";
    }
    out << '\n';
}

CommandOutcome
runCommand(lona::tooling::Session &session, std::string_view rawCommand,
           OutputFormat format, std::ostream &out) {
    auto command = trimCopy(rawCommand);
    if (command.empty()) {
        return {};
    }

    if (command == "quit" || command == "exit" || command == "q") {
        return CommandOutcome{false, 0};
    }

    if (command == "help") {
        if (format == OutputFormat::Json) {
            emitJsonResponse(out, true, command, helpJson());
        } else {
            printHelp(out);
        }
        return {};
    }

    if (command == "status") {
        if (format == OutputFormat::Json) {
            emitJsonResponse(out, true, command, session.statusJson());
        } else {
            printStatus(session, out);
        }
        return {};
    }

    if (command == "reload") {
        if (session.currentPath().empty()) {
            return emitCommandError(format, out, command,
                                    "reload requires an open source");
        }
        const auto reloaded = session.reload();
        if (!reloaded && !session.hasLoadedSource()) {
            return emitCommandError(
                format, out, command,
                loadFailureMessage(session, "reload failed"));
        }
        if (format == OutputFormat::Json) {
            emitJsonResponse(out, true, command, session.statusJson());
        } else {
            printLoadSummary(session, out);
        }
        return {};
    }

    if (command.rfind("goto ", 0) == 0) {
        auto lineText = trimCopy(std::string_view(command).substr(5));
        auto line = parsePositiveInt(lineText);
        if (!line) {
            return emitCommandError(format, out, command,
                                    "goto requires a positive line number");
        }
        std::string error;
        if (!session.gotoLine(*line, &error)) {
            return emitCommandError(format, out, command, error);
        }
        if (format == OutputFormat::Json) {
            emitJsonResponse(out, true, command, session.cursorJson());
        } else {
            auto cursor = session.cursorJson();
            out << "cursor: " << cursor["path"].get<std::string>() << ':'
                << cursor["line"].get<int>();
            if (cursor["hasLocalScope"].get<bool>()) {
                out << " in "
                    << cursor["context"]["kind"].get<std::string>() << ' '
                    << cursor["context"]["name"].get<std::string>();
            } else {
                out << " (no local scope)";
            }
            out << '\n';
        }
        return {};
    }

    if (command == "diagnostics" || command == "print diagnostics") {
        if (format == OutputFormat::Json) {
            emitJsonResponse(out, true, command, session.diagnosticsJson());
        } else {
            session.printDiagnostics(out);
        }
        return {};
    }

    if (command == "info local" || command.rfind("info local ", 0) == 0) {
        int line = 0;
        if (command != "info local") {
            auto lineText = trimCopy(std::string_view(command).substr(11));
            auto parsed = parsePositiveInt(lineText);
            if (!parsed) {
                return emitCommandError(
                    format, out, command,
                    "info local requires a positive line number");
            }
            std::string error;
            if (!session.gotoLine(*parsed, &error)) {
                return emitCommandError(format, out, command, error);
            }
            line = *parsed;
        }
        if (format == OutputFormat::Json) {
            emitJsonResponse(out, true, command, session.infoLocalJson(line));
        } else {
            session.printInfoLocal(out, line);
        }
        return {};
    }

    if (command == "ast" || command == "print ast") {
        if (format == OutputFormat::Json) {
            emitJsonResponse(out, true, command, session.astJson());
        } else {
            session.printAst(out);
        }
        return {};
    }

    if (command == "info global" || command == "symbols" ||
        command == "print symbols") {
        if (format == OutputFormat::Json) {
            emitJsonResponse(out, true, command, session.symbolsJson());
        } else {
            session.printSymbols(out);
        }
        return {};
    }

    if (command == "find" || command.rfind("find ", 0) == 0) {
        auto args = command == "find" ? std::string_view()
                                       : std::string_view(command).substr(5);
        auto [kind, pattern] = parseFilterAndPattern(args);
        if (format == OutputFormat::Json) {
            emitJsonResponse(out, true, command,
                             session.findResultsJson(kind, pattern));
        } else {
            session.printFindResults(out, kind, pattern);
        }
        return {};
    }

    if (command.rfind("open ", 0) == 0) {
        const auto path = trimCopy(std::string_view(command).substr(5));
        if (path.empty()) {
            return emitCommandError(format, out, command,
                                    "open requires a path");
        }
        const auto opened = session.openFile(path);
        if (!opened && !session.hasLoadedSource()) {
            return emitCommandError(
                format, out, command,
                loadFailureMessage(session, "open failed"));
        }
        if (format == OutputFormat::Json) {
            emitJsonResponse(out, true, command, session.statusJson());
        } else {
            printLoadSummary(session, out);
        }
        return {};
    }

    return emitCommandError(format, out, command,
                            "unknown command: " + command);
}

}  // namespace

int
main(int argc, char *argv[]) {
    cmdline::parser cli;
    cli.add<std::string>("source", 0, "load source text directly", false, "");
    cli.add<std::string>("path", 0,
                         "virtual source path used with --source",
                         false, "<memory>.lo");
    cli.add<int>("error-limit", 0,
                 "maximum diagnostics to collect before stopping", false, 20);
    cli.add<std::string>("format", 0, "output format", false, "text",
                         cmdline::oneof<std::string>("text", "json"));
    cli.add<std::string>("command", 0,
                         "run a single command and exit", false, "");

    cli.parse_check(argc, argv);
    const auto &args = cli.rest();
    if (args.size() > 1) {
        std::cerr << cli.usage();
        return 1;
    }

    const auto format =
        cli.get<std::string>("format") == "json" ? OutputFormat::Json
                                                  : OutputFormat::Text;

    lona::tooling::Session session(
        static_cast<std::size_t>(std::max(0, cli.get<int>("error-limit"))));

    if (cli.exist("source")) {
        session.setSourceText(cli.get<std::string>("path"),
                              cli.get<std::string>("source"));
    } else if (!args.empty()) {
        session.openFile(args.front());
    }

    if (cli.exist("command")) {
        auto outcome =
            runCommand(session, cli.get<std::string>("command"), format,
                       std::cout);
        return outcome.exitCode;
    }

    if (format == OutputFormat::Text && (cli.exist("source") || !args.empty())) {
        printLoadSummary(session, std::cout);
    }

    std::string line;
    while (true) {
        if (format == OutputFormat::Text) {
            std::cout << "lona-query> " << std::flush;
        }
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (trimCopy(line).empty()) {
            continue;
        }
        auto outcome = runCommand(session, line, format, std::cout);
        if (!outcome.keepRunning) {
            break;
        }
        if (outcome.exitCode != 0 && format == OutputFormat::Text) {
            std::cout << "type `help` for available commands\n";
        }
    }

    return 0;
}
