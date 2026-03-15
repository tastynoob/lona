#include "cmdline.hpp"
#include "lona/driver/session.hh"
#include "lona/err/err.hh"
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <string>

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

}  // namespace

int
main(int argc, char *argv[]) {
    cmdline::parser cli;
    cli.add("emit-ir", 'S', "print LLVM IR instead of AST JSON");
    cli.add("verify-ir", 0, "verify generated LLVM IR before printing");
    cli.add("debug", 'g', "emit LLVM debug metadata");
    cli.add("stats", 0, "print per-phase compile statistics to stderr");
    cli.add<int>("opt", 'O', "LLVM optimization level (0-3)", false, 0,
                 cmdline::range(0, 3));
    cli.parse_check(argc, argv);

    const auto &args = cli.rest();
    if (args.empty() || args.size() > 2) {
        std::cerr << cli.usage();
        return 1;
    }

    const std::string &inputPath = args[0];
    lona::CompilerSession session;

    std::ostream *out = &std::cout;
    std::ofstream output;
    if (args.size() == 2) {
        output.open(args[1]);
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
    const bool compileMode = cli.exist("emit-ir") || cli.exist("verify-ir") ||
                             cli.exist("debug") || cli.exist("opt");
    options.outputMode =
        compileMode ? lona::OutputMode::LLVMIR : lona::OutputMode::AstJson;
    options.compile.optLevel = cli.get<int>("opt");
    options.compile.verifyIR = cli.exist("verify-ir");
    options.compile.debugInfo = cli.exist("debug");

    int exitCode = session.runFile(inputPath, options, *out, std::cerr);
    if (cli.exist("stats")) {
        session.printStats(std::cerr);
    }
    finishProcess(exitCode, out);
}
