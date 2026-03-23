#include "lona/driver/session.hh"
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace lona {
using Json = nlohmann::json;

namespace {

SessionOptions
buildOptions(const Json &command) {
    SessionOptions options;
    const std::string outputMode =
        command.value("output_mode", std::string("llvm_ir"));
    options.outputMode = outputMode == "ast_json" ? OutputMode::AstJson
                                                   : OutputMode::LLVMIR;
    options.compile.verifyIR = command.value("verify_ir", false);
    options.compile.debugInfo = command.value("debug", false);
    options.compile.optLevel = command.value("opt_level", 0);
    options.compile.targetTriple = command.value("target", std::string());
    return options;
}

Json
compileWithSession(CompilerSession &session, const Json &command) {
    Json result = Json::object();
    const auto input = command.at("input").get<std::string>();
    std::ostringstream out;
    std::ostringstream diag;
    const int exitCode =
        session.runFile(input, buildOptions(command), out, diag);

    result["command"] = "compile";
    result["input"] = input;
    result["exit_code"] = exitCode;
    result["stdout"] = out.str();
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
    result["stats"] = std::move(stats);
    return result;
}

}  // namespace
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
                reply = lona::compileWithSession(*session, command);
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
