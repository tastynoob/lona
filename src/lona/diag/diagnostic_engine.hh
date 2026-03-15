#pragma once

#include "lona/err/err.hh"
#include "lona/source/source_manager.hh"
#include <ostream>
#include <string>

namespace lona {

class DiagnosticEngine {
    const SourceManager *sources_ = nullptr;

public:
    explicit DiagnosticEngine(const SourceManager *sources = nullptr)
        : sources_(sources) {}

    void setSourceManager(const SourceManager *sources) { sources_ = sources; }

    std::string render(const DiagnosticError &error,
                       const std::string &fallbackPath = std::string()) const;
    void emit(const DiagnosticError &error, std::ostream &out,
              const std::string &fallbackPath = std::string()) const;
};

}  // namespace lona
