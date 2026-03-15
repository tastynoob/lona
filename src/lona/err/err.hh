#pragma once

#include "location.hh"
#include <stdexcept>
#include <string>
#include <vector>

namespace lona {

class DiagnosticError : public std::runtime_error {
public:
    enum class Category {
        Lexical,
        Syntax,
        Semantic,
        Internal,
    };

private:
    Category category_;
    location loc_;
    bool hasLocation_ = false;
    std::string hint_;

public:
    DiagnosticError(Category category, std::string message,
                    std::string hint = std::string());
    DiagnosticError(Category category, const location &loc, std::string message,
                    std::string hint = std::string());

    Category category() const { return category_; }
    bool hasLocation() const { return hasLocation_; }
    const location &where() const { return loc_; }
    const std::string &hint() const { return hint_; }
};

class Err {
    std::string path;
    std::vector<std::string> lines;

public:
    explicit Err(const std::string &sourcePath = std::string());

    std::string render(const DiagnosticError &error) const;
};

std::string formatDiagnostic(const DiagnosticError &error,
                             const std::string &fallbackPath = std::string());
std::string friendlySyntaxMessage(const std::string &rawMessage);

}  // namespace lona
