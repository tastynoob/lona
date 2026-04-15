#include "cmdline.hpp"
#include "tooling/command.hh"
#include "tooling/line_editor.hh"
#include "tooling/output.hh"
#include "tooling/session.hh"
#include <algorithm>
#include <iostream>
#include <optional>
#include <string>

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
    if (cli.exist("source") && !args.empty()) {
        std::cerr << cli.usage();
        return 1;
    }
    if (args.empty() == false && args.front().empty()) {
        std::cerr << cli.usage();
        return 1;
    }

    const auto format =
        cli.get<std::string>("format") == "json"
            ? lona::tooling::OutputFormat::Json
            : lona::tooling::OutputFormat::Text;

    lona::tooling::Session session(
        static_cast<std::size_t>(std::max(0, cli.get<int>("error-limit"))));
    lona::tooling::OutputFormatter formatter(format, std::cout);
    const auto commands = lona::tooling::buildCommandRegistry();

    if (cli.exist("source")) {
        session.setSourceText(cli.get<std::string>("path"),
                              cli.get<std::string>("source"));
    } else if (!args.empty()) {
        session.setRootPaths(std::vector<std::string>(args.begin(), args.end()));
    }

    if (cli.exist("command")) {
        auto outcome =
            commands.dispatch(session, cli.get<std::string>("command"),
                              formatter);
        return outcome.exitCode;
    }

    if (!formatter.isJson() && (cli.exist("source") || !args.empty())) {
        formatter.emitLoadSummary("startup", session);
    }

    std::optional<lona::tooling::LineEditor> lineEditor;
    if (!formatter.isJson() && lona::tooling::LineEditor::supported()) {
        lineEditor.emplace(formatter.promptText());
    }

    while (true) {
        std::string line;
        if (lineEditor.has_value()) {
            auto edited = lineEditor->readLine();
            if (!edited.has_value()) {
                break;
            }
            line = std::move(*edited);
        } else {
            formatter.printPrompt();
            if (!std::getline(std::cin, line)) {
                break;
            }
        }
        if (lona::tooling::trimCopy(line).empty()) {
            continue;
        }
        auto outcome = commands.dispatch(session, line, formatter);
        if (!outcome.keepRunning) {
            break;
        }
        if (outcome.exitCode != 0) {
            formatter.printRetryHint();
        }
    }

    return 0;
}
