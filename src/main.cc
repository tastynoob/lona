#include "cmdline.hpp"
#include "lona/driver/session.hh"
#include "lona/err/err.hh"
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

[[noreturn]] void
flushProcessStreamsAndExit(int exitCode, std::ostream *out = nullptr) {
    if (out != nullptr) {
        out->flush();
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(exitCode);
}

std::vector<std::string>
normalizeMainCliArgs(int argc, char *argv[]) {
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

int
main(int argc, char *argv[]) {
    cmdline::parser cli;
    cli.add<std::string>(
        "emit", 0,
        "select output artifact: ir, entry (hosted entry object), obj (single final object), or objects (module object bundle)",
        false, "",
        cmdline::oneof<std::string>("ir", "entry", "obj", "objects"));
    cli.add<std::string>(
        "target", 0,
        "LLVM target triple, for example x86_64-none-elf or x86_64-unknown-linux-gnu",
        false, "");
    cli.add<std::string>(
        "cache-dir", 0,
        "output directory for `--emit objects` bundle members",
        false, "./lona_cache");
    cli.add<std::string>(
        "lto", 0, "link-time optimization mode: off or full", false, "off",
        cmdline::oneof<std::string>("off", "full"));
    cli.add("no-cache", 0, "disable module artifact reuse for this compile");
    cli.add("verify-ir", 0, "verify generated LLVM IR before printing");
    cli.add("debug", 'g', "emit LLVM debug metadata");
    cli.add("stats", 0, "print per-phase compile statistics to stderr");
    cli.add<int>("opt", 'O', "LLVM optimization level (0-3)", false, 0,
                 cmdline::range(0, 3));
    auto normalizedArgs = normalizeMainCliArgs(argc, argv);
    cli.parse_check(normalizedArgs);

    const auto &args = cli.rest();
    if (args.size() > 2) {
        std::cerr << cli.usage();
        return 1;
    }

    lona::CompilerSession session;
    const std::string emitTarget =
        cli.exist("emit") ? cli.get<std::string>("emit") : std::string();
    const bool emitIR = emitTarget == "ir";
    const bool emitEntry = emitTarget == "entry";
    const bool emitObject = emitTarget == "obj";
    const bool emitObjects = emitTarget == "objects";
    const std::string ltoMode = cli.get<std::string>("lto");

    if (emitEntry) {
        if (args.size() != 1) {
            std::cerr << "`--emit entry` requires an explicit output object path\n";
            std::cerr << cli.usage();
            return 1;
        }
    } else if (args.empty() || args.size() > 2) {
        std::cerr << cli.usage();
        return 1;
    }

    if (emitObjects && args.size() != 2) {
        std::cerr << "`--emit objects` requires an explicit manifest output path\n";
        std::cerr << cli.usage();
        return 1;
    }
    if (emitObjects && ltoMode != "off") {
        std::cerr << "`--emit objects` does not support `--lto " << ltoMode
                  << "`\n";
        std::cerr << cli.usage();
        return 1;
    }
    if (!emitObjects && cli.exist("cache-dir")) {
        std::cerr << "`--cache-dir` is only supported with `--emit objects`\n";
        std::cerr << cli.usage();
        return 1;
    }
    if (emitEntry && ltoMode != "off") {
        std::cerr << "`--emit entry` does not support `--lto " << ltoMode << "`\n";
        std::cerr << cli.usage();
        return 1;
    }

    std::ostream *out = &std::cout;
    std::ofstream output;
    const std::string inputPath =
        (!emitEntry && !args.empty()) ? args[0] : std::string();
    const std::string outputPath =
        emitEntry ? args[0] : (args.size() == 2 ? args[1] : std::string());
    const bool builderWritesOutputDirectly =
        !outputPath.empty() && (emitEntry || emitObject);
    if (!outputPath.empty() && !builderWritesOutputDirectly) {
        std::ios::openmode fileMode = std::ios::out;
        if (emitEntry || emitObject) {
            fileMode |= std::ios::binary;
        }
        output.open(outputPath, fileMode);
        if (!output) {
            session.diagnostics().emit(
                lona::DiagnosticError(
                    lona::DiagnosticError::Category::Driver,
                    "I couldn't open output file `" + outputPath + "`.",
                    "Check that the path is writable and that parent directories exist."),
                std::cerr);
            flushProcessStreamsAndExit(1);
        }
        out = &output;
    }

    lona::SessionOptions options;
    const bool compileMode = emitIR || emitEntry || emitObject || emitObjects ||
                             cli.exist("no-cache") ||
                             cli.exist("verify-ir") ||
                             cli.exist("debug") || cli.exist("opt") ||
                             cli.exist("target") || ltoMode != "off";
    if (emitObjects) {
        options.outputMode = lona::OutputMode::ObjectBundle;
    } else if (emitEntry) {
        options.outputMode = lona::OutputMode::EntryObject;
    } else if (emitObject) {
        options.outputMode = lona::OutputMode::ObjectFile;
    } else if (compileMode) {
        options.outputMode = lona::OutputMode::LLVMIR;
    } else {
        options.outputMode = lona::OutputMode::AstJson;
    }
    options.outputPath = outputPath;
    options.cacheOutputPath =
        emitObjects ? cli.get<std::string>("cache-dir") : std::string();
    options.compile.optLevel = cli.get<int>("opt");
    options.compile.noCache = cli.exist("no-cache");
    options.compile.verifyIR = cli.exist("verify-ir");
    options.compile.debugInfo = cli.exist("debug");
    options.compile.targetTriple =
        cli.exist("target") ? cli.get<std::string>("target") : std::string();
    options.compile.ltoMode = ltoMode == "full"
        ? lona::CompileOptions::LTOMode::Full
        : lona::CompileOptions::LTOMode::Off;

    int exitCode = emitEntry
        ? session.runEntry(options, *out, std::cerr)
        : session.runFile(inputPath, options, *out, std::cerr);
    if (cli.exist("stats")) {
        session.printStats(std::cerr);
    }
    flushProcessStreamsAndExit(exitCode, out);
}
