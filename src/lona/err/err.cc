#include "err.hh"
#include <utility>

namespace lona {
namespace {

std::string
replaceAll(std::string input, const std::string &from, const std::string &to) {
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
trimTrailingPunctuation(std::string input) {
    while (!input.empty() && (input.back() == '.' || input.back() == ' ')) {
        input.pop_back();
    }
    return input;
}

std::string
displayTokenText(const std::string &text) {
    if (text == "\n") {
        return "\\n";
    }
    if (text == "\t") {
        return "\\t";
    }
    return text;
}

}  // namespace

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
        detail = replaceAll(detail, ", expecting ", "; expected ");
        detail = replaceAll(detail, " or ", ", ");
        detail = trimTrailingPunctuation(detail);
        return "I couldn't parse this statement: unexpected " + detail + '.';
    }

    return "I couldn't parse this statement: " +
           trimTrailingPunctuation(displayTokenText(rawMessage)) + '.';
}

}  // namespace lona
