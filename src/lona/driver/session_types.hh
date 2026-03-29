#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lona {

struct CompileOptions {
    enum class LTOMode {
        Off,
        Full,
    };

    int optLevel = 0;
    bool verifyIR = false;
    bool debugInfo = false;
    bool noCache = false;
    std::string targetTriple;
    std::vector<std::string> includePaths;
    LTOMode ltoMode = LTOMode::Off;
};

enum class OutputMode {
    AstJson,
    LLVMIR,
    EntryObject,
    ObjectFile,
    ObjectBundle,
};

struct SessionOptions {
    OutputMode outputMode = OutputMode::AstJson;
    std::string outputPath;
    std::string cacheOutputPath;
    CompileOptions compile;
};

struct SessionStats {
    std::size_t loadedUnits = 0;
    double parseMs = 0.0;
    double dependencyScanMs = 0.0;
    double declarationMs = 0.0;
    double dependencyDeclarationMs = 0.0;
    double entryDeclarationMs = 0.0;
    double lowerMs = 0.0;
    double resolveMs = 0.0;
    double analyzeMs = 0.0;
    double codegenMs = 0.0;
    double emitLlvmMs = 0.0;
    double artifactEmitMs = 0.0;
    double outputEmitMs = 0.0;
    double outputRenderMs = 0.0;
    double outputWriteMs = 0.0;
    double cacheLookupMs = 0.0;
    double cacheRestoreMs = 0.0;
    double optimizeMs = 0.0;
    double moduleOptimizeMs = 0.0;
    double ltoOptimizeMs = 0.0;
    double verifyMs = 0.0;
    double moduleVerifyMs = 0.0;
    double linkVerifyMs = 0.0;
    double linkMs = 0.0;
    double linkLoadMs = 0.0;
    double linkMergeMs = 0.0;
    double totalMs = 0.0;
    std::size_t compiledModules = 0;
    std::size_t reusedModules = 0;
    std::size_t emittedModuleBitcode = 0;
    std::size_t reusedModuleBitcode = 0;
    std::size_t emittedModuleObjects = 0;
    std::size_t reusedModuleObjects = 0;
};

}  // namespace lona
