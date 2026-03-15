#include "err.hh"
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <utility>

namespace lona {
namespace {

std::string
categoryLabel(DiagnosticError::Category category) {
    switch (category) {
    case DiagnosticError::Category::Lexical:
        return "lexical error";
    case DiagnosticError::Category::Syntax:
        return "syntax error";
    case DiagnosticError::Category::Semantic:
        return "semantic error";
    case DiagnosticError::Category::Internal:
        return "internal error";
    }
    return "error";
}

bool
hasUsableLocation(const location &loc) {
    return loc.begin.line > 0 && loc.begin.column > 0;
}

std::string
locationPath(const DiagnosticError &error, const std::string &fallbackPath) {
    if (error.hasLocation() && error.where().begin.filename) {
        return *error.where().begin.filename;
    }
    return fallbackPath;
}

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

std::string
underlineFor(const location &loc, const std::string &line) {
    const int startColumn = std::max(1, loc.begin.column);
    int width = 1;
    if (loc.begin.line == loc.end.line && loc.end.column > loc.begin.column) {
        width = loc.end.column - loc.begin.column;
    } else if (static_cast<std::size_t>(startColumn) <= line.size()) {
        width = static_cast<int>(line.size() - static_cast<std::size_t>(startColumn) + 1);
    }
    return std::string(static_cast<std::size_t>(startColumn - 1), ' ') +
           std::string(static_cast<std::size_t>(std::max(1, width)), '^');
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

Err::Err(const std::string &sourcePath) : path(sourcePath) {
    if (path.empty()) {
        return;
    }

    std::ifstream input(path);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
}

std::string
Err::render(const DiagnosticError &error) const {
    std::ostringstream out;
    out << categoryLabel(error.category()) << ": " << error.what() << '\n';

    if (!error.hasLocation() || !hasUsableLocation(error.where())) {
        if (!error.hint().empty()) {
            out << "help: " << error.hint() << '\n';
        }
        return out.str();
    }

    const auto &loc = error.where();
    const auto lineNumber = loc.begin.line;
    const auto columnNumber = loc.begin.column;

    if (!path.empty()) {
        out << " --> " << path << ':' << lineNumber << ':' << columnNumber << '\n';
    } else {
        out << " --> " << lineNumber << ':' << columnNumber << '\n';
    }

    if (lineNumber > 0 && static_cast<std::size_t>(lineNumber) <= lines.size()) {
        const auto &sourceLine = lines[static_cast<std::size_t>(lineNumber - 1)];
        const auto lineLabel = std::to_string(lineNumber);
        out << ' ' << std::string(lineLabel.size(), ' ') << " |\n";
        out << ' ' << lineLabel << " | " << sourceLine << '\n';
        out << ' ' << std::string(lineLabel.size(), ' ') << " | "
            << underlineFor(loc, sourceLine) << '\n';
    }

    if (!error.hint().empty()) {
        out << "help: " << error.hint() << '\n';
    }
    return out.str();
}

std::string
formatDiagnostic(const DiagnosticError &error, const std::string &fallbackPath) {
    return Err(locationPath(error, fallbackPath)).render(error);
}

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
