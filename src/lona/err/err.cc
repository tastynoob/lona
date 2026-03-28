#include "err.hh"
#include <utility>

namespace lona {

std::string
replaceAllInDiagnosticText(std::string input, const std::string &from,
                           const std::string &to) {
    if (from.empty()) {
        return input;
    }
    std::size_t pos = 0;
    while ((pos = input.find(from, pos)) != std::string::npos) {
        input.replace(pos, from.size(), to);
        pos += to.size();
    }
    return input;
}

std::string
trimDiagnosticTrailingPunctuation(std::string input) {
    while (!input.empty() && (input.back() == '.' || input.back() == ' ')) {
        input.pop_back();
    }
    return input;
}

std::string
displayDiagnosticTokenText(const std::string &text) {
    if (text == "\n") {
        return "\\n";
    }
    if (text == "\t") {
        return "\\t";
    }
    return text;
}

DiagnosticError::DiagnosticError(Category category, std::string message,
                                 std::string hint)
    : std::runtime_error(std::move(message)),
      category_(category),
      hint_(std::move(hint)) {}

DiagnosticError::DiagnosticError(Category category, const location &loc,
                                 std::string message, std::string hint)
    : std::runtime_error(std::move(message)),
      category_(category),
      loc_(loc),
      hasLocation_(true),
      hint_(std::move(hint)) {}

std::string
friendlySyntaxMessage(const std::string &rawMessage) {
    if (rawMessage.empty() || rawMessage == "syntax error") {
        return "I couldn't parse this statement.";
    }

    constexpr char prefix[] = "syntax error, unexpected ";
    if (rawMessage.rfind(prefix, 0) == 0) {
        auto detail = rawMessage.substr(sizeof(prefix) - 1);
        detail = replaceAllInDiagnosticText(detail, ", expecting ", "; expected ");
        detail = replaceAllInDiagnosticText(detail, " or ", ", ");
        detail = trimDiagnosticTrailingPunctuation(detail);
        return "I couldn't parse this statement: unexpected " + detail + '.';
    }

    return "I couldn't parse this statement: " +
           trimDiagnosticTrailingPunctuation(
               displayDiagnosticTokenText(rawMessage)) +
           '.';
}

}  // namespace lona
