#include "lona/driver/session.hh"
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace lona {
using Json = nlohmann::json;

SessionOptions
buildSessionRunnerOptions(const Json &command) {
    SessionOptions options;
    const std::string outputMode =
        command.value("output_mode", std::string("llvm_ir"));
    if (outputMode == "ast_json") {
        options.outputMode = OutputMode::AstJson;
    } else if (outputMode == "linked_object") {
        options.outputMode = OutputMode::LinkedObject;
    } else if (outputMode == "bitcode_bundle") {
        options.outputMode = OutputMode::BitcodeBundle;
    } else if (outputMode == "object_bundle") {
        options.outputMode = OutputMode::ObjectBundle;
    } else {
        options.outputMode = OutputMode::LLVMIR;
    }
    const std::string ltoMode = command.value("lto", std::string("off"));
    options.compile.verifyIR = command.value("verify_ir", false);
    options.compile.debugInfo = command.value("debug", false);
    options.compile.noCache = command.value("no_cache", false);
    options.compile.optLevel = command.value("opt_level", 0);
    options.compile.targetTriple = command.value("target", std::string());
    options.compile.includePaths =
        command.value("include_paths", std::vector<std::string>{});
    options.compile.ltoMode = ltoMode == "full" ? CompileOptions::LTOMode::Full
                                                 : CompileOptions::LTOMode::Off;
    return options;
}

Json
compileSessionRunnerCommand(CompilerSession &session, const Json &command) {
    Json result = Json::object();
    const auto input = command.at("input").get<std::string>();
    auto options = buildSessionRunnerOptions(command);
    if (options.outputMode == OutputMode::BitcodeBundle ||
        options.outputMode == OutputMode::ObjectBundle) {
        const std::string requestedOutput =
            command.value("output_path", std::string());
        const auto manifestPath = requestedOutput.empty()
            ? (std::filesystem::path(input).parent_path() / "session-runner.manifest")
            : std::filesystem::path(requestedOutput);
        options.outputPath = manifestPath.string();
        result["output_path"] = manifestPath.string();
    }
    std::ostringstream out;
    std::ostringstream diag;
    const int exitCode = session.runFile(input, options, out, diag);

    result["command"] = "compile";
    result["input"] = input;
    result["exit_code"] = exitCode;
    const std::string output = out.str();
    result["stdout_size"] = output.size();
    if (options.outputMode == OutputMode::LinkedObject) {
        result["stdout"] = "";
    } else {
        result["stdout"] = output;
    }
    result["stderr"] = diag.str();

    Json stats = Json::object();
    const auto &lastStats = session.lastStats();
    stats["loaded_units"] = lastStats.loadedUnits;
    stats["parse_ms"] = lastStats.parseMs;
    stats["declaration_ms"] = lastStats.declarationMs;
    stats["lower_ms"] = lastStats.lowerMs;
    stats["codegen_ms"] = lastStats.codegenMs;
    stats["optimize_ms"] = lastStats.optimizeMs;
    stats["verify_ms"] = lastStats.verifyMs;
    stats["link_ms"] = lastStats.linkMs;
    stats["total_ms"] = lastStats.totalMs;
    stats["compiled_modules"] = lastStats.compiledModules;
    stats["reused_modules"] = lastStats.reusedModules;
    stats["emitted_module_bitcode"] = lastStats.emittedModuleBitcode;
    stats["reused_module_bitcode"] = lastStats.reusedModuleBitcode;
    stats["emitted_module_objects"] = lastStats.emittedModuleObjects;
    stats["reused_module_objects"] = lastStats.reusedModuleObjects;
    result["stats"] = std::move(stats);
    return result;
}
}  // namespace lona

int
main() {
    using lona::CompilerSession;
    using lona::Json;

    std::unique_ptr<CompilerSession> session = std::make_unique<CompilerSession>();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        Json reply = Json::object();
        try {
            const Json command = Json::parse(line);
            const auto kind = command.value("command", std::string("compile"));
            if (kind == "compile") {
                reply = lona::compileSessionRunnerCommand(*session, command);
            } else if (kind == "reset_session") {
                session = std::make_unique<CompilerSession>();
                reply["command"] = "reset_session";
                reply["ok"] = true;
            } else {
                reply["command"] = kind;
                reply["ok"] = false;
                reply["error"] = "unknown command";
            }
        } catch (const std::exception &ex) {
            reply["command"] = "error";
            reply["ok"] = false;
            reply["error"] = ex.what();
        }

        std::cout << reply.dump() << '\n';
        std::cout.flush();
    }
    return 0;
}
