#include "cmdline.hpp"
#include "lona/driver/session.hh"
#include "lona/err/err.hh"
#include "lona/version.hh"
#include <cstdlib>
#include <fstream>
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

struct MainCliArgs {
    std::vector<std::string> args;
    std::vector<std::string> includePaths;
    std::string error;
};

MainCliArgs
normalizeMainCliArgs(int argc, char *argv[]) {
    MainCliArgs result;
    result.args.reserve(static_cast<size_t>(argc) + 4);

    for (int i = 0; i < argc; ++i) {
        std::string arg = argv[i];
        if (i == 0) {
            result.args.push_back(std::move(arg));
            continue;
        }
        if (i != 0 && arg.size() > 2 && arg[0] == '-' && arg[1] == 'O') {
            result.args.push_back("-O");
            result.args.push_back(arg.substr(2));
            continue;
        }
        if (arg == "-I" || arg == "--include-dir") {
            if (i + 1 >= argc) {
                result.error = "option needs value: " + arg;
                return result;
            }
            result.includePaths.push_back(argv[++i]);
            continue;
        }
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == 'I') {
            result.includePaths.push_back(arg.substr(2));
            continue;
        }
        if (arg.rfind("--include-dir=", 0) == 0) {
            result.includePaths.push_back(
                arg.substr(std::string("--include-dir=").size()));
            continue;
        }
        result.args.push_back(std::move(arg));
    }

    return result;
}

int
main(int argc, char *argv[]) {
    cmdline::parser cli;
    cli.add<std::string>(
        "emit", 0,
        "select output artifact: ir, bc (module bitcode bundle), obj (module "
        "object bundle), linked-bc (single final linked bitcode), mbc "
        "(single final managed linked bitcode), linked-obj (single final "
        "object), or entry (hosted entry object)",
        false, "",
        cmdline::oneof<std::string>("ir", "entry", "bc", "obj",
                                    "linked-bc", "mbc", "linked-obj"));
    cli.add<std::string>("target", 0,
                         "LLVM target triple, for example x86_64-none-elf or "
                         "x86_64-unknown-linux-gnu",
                         false, "");
    cli.add<std::string>(
        "cache-dir", 0,
        "artifact cache directory for module bundles and linked-bitcode / "
        "linked-object bitcode intermediates",
        false, "./lona_cache");
    cli.add<std::string>(
        "include-dir", 'I',
        "add a module include search directory; searched after the importing "
        "file directory and may be repeated",
        false, "");
    cli.add<std::string>("lto", 0, "link-time optimization mode: off or full",
                         false, "off",
                         cmdline::oneof<std::string>("off", "full"));
    cli.add("no-cache", 0, "disable module artifact reuse for this compile");
    cli.add("verify-ir", 0, "verify generated LLVM IR before printing");
    cli.add("debug", 'g', "emit LLVM debug metadata");
    cli.add("stats", 0, "print per-phase compile statistics to stderr");
    cli.add("version", 0, "print language version and compiler revision");
    cli.add<int>("opt", 'O', "LLVM optimization level (0-3)", false, 0,
                 cmdline::range(0, 3));
    auto normalizedArgs = normalizeMainCliArgs(argc, argv);
    if (!normalizedArgs.error.empty()) {
        std::cerr << normalizedArgs.error << '\n';
        std::cerr << cli.usage();
        return 1;
    }
    cli.parse_check(normalizedArgs.args);
    if (cli.exist("version")) {
        std::cout << lona::versionString() << '\n';
        flushProcessStreamsAndExit(0, &std::cout);
    }

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
    const bool emitBitcodeBundle = emitTarget == "bc";
    const bool emitObject = emitTarget == "obj";
    const bool emitLinkedBitcode = emitTarget == "linked-bc";
    const bool emitManagedBitcode = emitTarget == "mbc";
    const bool emitLinkedObject = emitTarget == "linked-obj";
    const std::string ltoMode = cli.get<std::string>("lto");

    if (emitEntry) {
        if (args.size() != 1) {
            std::cerr
                << "`--emit entry` requires an explicit output object path\n";
            std::cerr << cli.usage();
            return 1;
        }
    } else if (args.empty() || args.size() > 2) {
        std::cerr << cli.usage();
        return 1;
    }

    const bool emitBundle = emitBitcodeBundle || emitObject;
    if (emitBundle && args.size() != 2) {
        std::cerr
            << "`--emit " << emitTarget
            << "` requires an explicit manifest output path\n";
        std::cerr << cli.usage();
        return 1;
    }
    if (emitBundle && ltoMode != "off") {
        std::cerr << "`--emit " << emitTarget << "` does not support `--lto "
                  << ltoMode
                  << "`\n";
        std::cerr << cli.usage();
        return 1;
    }
    if (!(emitBundle || emitLinkedBitcode || emitManagedBitcode ||
          emitLinkedObject) &&
        cli.exist("cache-dir")) {
        std::cerr << "`--cache-dir` is only supported with `--emit bc`, "
                     "`--emit obj`, `--emit linked-bc`, `--emit mbc`, or "
                     "`--emit linked-obj`\n";
        std::cerr << cli.usage();
        return 1;
    }
    if (emitEntry && ltoMode != "off") {
        std::cerr << "`--emit entry` does not support `--lto " << ltoMode
                  << "`\n";
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
        !outputPath.empty() &&
        (emitEntry || emitLinkedBitcode || emitManagedBitcode ||
         emitLinkedObject);
    if (!outputPath.empty() && !builderWritesOutputDirectly) {
        std::ios::openmode fileMode = std::ios::out;
        if (emitEntry || emitLinkedObject) {
            fileMode |= std::ios::binary;
        }
        output.open(outputPath, fileMode);
        if (!output) {
            session.diagnostics().emit(
                lona::DiagnosticError(
                    lona::DiagnosticError::Category::Driver,
                    "I couldn't open output file `" + outputPath + "`.",
                    "Check that the path is writable and that parent "
                    "directories exist."),
                std::cerr);
            flushProcessStreamsAndExit(1);
        }
        out = &output;
    }

    lona::SessionOptions options;
    const bool compileMode =
        emitIR || emitEntry || emitBitcodeBundle || emitObject ||
        emitLinkedBitcode || emitManagedBitcode || emitLinkedObject ||
        cli.exist("no-cache") ||
        cli.exist("verify-ir") || cli.exist("debug") || cli.exist("opt") ||
        cli.exist("target") || ltoMode != "off";
    if (emitBitcodeBundle) {
        options.outputMode = lona::OutputMode::BitcodeBundle;
    } else if (emitObject) {
        options.outputMode = lona::OutputMode::ObjectBundle;
    } else if (emitEntry) {
        options.outputMode = lona::OutputMode::EntryObject;
    } else if (emitLinkedBitcode) {
        options.outputMode = lona::OutputMode::LinkedBitcode;
    } else if (emitManagedBitcode) {
        options.outputMode = lona::OutputMode::ManagedBitcode;
    } else if (emitLinkedObject) {
        options.outputMode = lona::OutputMode::LinkedObject;
    } else if (compileMode) {
        options.outputMode = lona::OutputMode::LLVMIR;
    } else {
        options.outputMode = lona::OutputMode::AstJson;
    }
    options.outputPath = outputPath;
    options.artifactCachePath =
        (emitBundle || emitLinkedBitcode || emitManagedBitcode ||
         emitLinkedObject)
            ? cli.get<std::string>("cache-dir")
            : (cli.exist("cache-dir") ? cli.get<std::string>("cache-dir")
                                      : std::string());
    options.compile.optLevel = cli.get<int>("opt");
    options.compile.noCache = cli.exist("no-cache");
    options.compile.verifyIR = cli.exist("verify-ir");
    options.compile.debugInfo = cli.exist("debug");
    options.compile.managedMode = emitManagedBitcode;
    options.compile.targetTriple =
        cli.exist("target") ? cli.get<std::string>("target") : std::string();
    options.compile.includePaths = std::move(normalizedArgs.includePaths);
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
