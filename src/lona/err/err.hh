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
        Driver,
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

std::string friendlySyntaxMessage(const std::string &rawMessage);

[[noreturn]] inline void
error(const std::string &message) {
    throw DiagnosticError(DiagnosticError::Category::Semantic, message);
}

[[noreturn]] inline void
error(const location &loc, const std::string &message,
      const std::string &hint = std::string()) {
    throw DiagnosticError(DiagnosticError::Category::Semantic, loc, message, hint);
}

[[noreturn]] inline void
internalError(const std::string &message,
              const std::string &hint = std::string()) {
    throw DiagnosticError(DiagnosticError::Category::Internal, message, hint);
}

[[noreturn]] inline void
internalError(const location &loc, const std::string &message,
              const std::string &hint = std::string()) {
    throw DiagnosticError(DiagnosticError::Category::Internal, loc, message, hint);
}

}  // namespace lona
