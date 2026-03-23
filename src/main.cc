#include "cmdline.hpp"
#include "lona/driver/session.hh"
#include "lona/err/err.hh"
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

[[noreturn]] void
finishProcess(int exitCode, std::ostream *out = nullptr) {
    if (out != nullptr) {
        out->flush();
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(exitCode);
}

std::vector<std::string>
normalizeCliArgs(int argc, char *argv[]) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc) + 4);

    for (int i = 0; i < argc; ++i) {
        std::string arg = argv[i];
        if (i != 0 && arg.size() > 2 && arg[0] == '-' && arg[1] == 'O') {
            args.push_back("-O");
            args.push_back(arg.substr(2));
            continue;
        }
        args.push_back(std::move(arg));
    }

    return args;
}

}  // namespace

int
main(int argc, char *argv[]) {
    cmdline::parser cli;
    cli.add<std::string>("emit", 0, "select output artifact: ir or obj", false, "",
                         cmdline::oneof<std::string>("ir", "obj"));
    cli.add("verify-ir", 0, "verify generated LLVM IR before printing");
    cli.add("debug", 'g', "emit LLVM debug metadata");
    cli.add("stats", 0, "print per-phase compile statistics to stderr");
    cli.add<int>("opt", 'O', "LLVM optimization level (0-3)", false, 0,
                 cmdline::range(0, 3));
    auto normalizedArgs = normalizeCliArgs(argc, argv);
    cli.parse_check(normalizedArgs);

    const auto &args = cli.rest();
    if (args.empty() || args.size() > 2) {
        std::cerr << cli.usage();
        return 1;
    }

    const std::string &inputPath = args[0];
    lona::CompilerSession session;
    const std::string emitTarget =
        cli.exist("emit") ? cli.get<std::string>("emit") : std::string();
    const bool emitIR = emitTarget == "ir";
    const bool emitObject = emitTarget == "obj";

    std::ostream *out = &std::cout;
    std::ofstream output;
    if (args.size() == 2) {
        std::ios::openmode fileMode = std::ios::out;
        if (emitObject) {
            fileMode |= std::ios::binary;
        }
        output.open(args[1], fileMode);
        if (!output) {
            session.diagnostics().emit(
                lona::DiagnosticError(
                    lona::DiagnosticError::Category::Driver,
                    "I couldn't open output file `" + args[1] + "`.",
                    "Check that the path is writable and that parent directories exist."),
                std::cerr);
            finishProcess(1);
        }
        out = &output;
    }

    lona::SessionOptions options;
    const bool compileMode = emitIR || emitObject || cli.exist("verify-ir") ||
                             cli.exist("debug") || cli.exist("opt");
    options.outputMode =
        emitObject ? lona::OutputMode::ObjectFile
                   : compileMode ? lona::OutputMode::LLVMIR
                                 : lona::OutputMode::AstJson;
    options.compile.optLevel = cli.get<int>("opt");
    options.compile.verifyIR = cli.exist("verify-ir");
    options.compile.debugInfo = cli.exist("debug");

    int exitCode = session.runFile(inputPath, options, *out, std::cerr);
    if (cli.exist("stats")) {
        session.printStats(std::cerr);
    }
    finishProcess(exitCode, out);
}
