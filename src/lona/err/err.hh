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

}  // namespace lona
