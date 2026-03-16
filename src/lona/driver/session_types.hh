#pragma once

#include <cstddef>

namespace lona {

struct CompileOptions {
    int optLevel = 0;
    bool verifyIR = false;
    bool debugInfo = false;
};

enum class OutputMode {
    AstJson,
    LLVMIR,
};

struct SessionOptions {
    OutputMode outputMode = OutputMode::AstJson;
    CompileOptions compile;
};

struct SessionStats {
    std::size_t loadedUnits = 0;
    double parseMs = 0.0;
    double declarationMs = 0.0;
    double lowerMs = 0.0;
    double codegenMs = 0.0;
    double optimizeMs = 0.0;
    double verifyMs = 0.0;
    double linkMs = 0.0;
    double totalMs = 0.0;
    std::size_t compiledModules = 0;
    std::size_t reusedModules = 0;
};

}  // namespace lona
