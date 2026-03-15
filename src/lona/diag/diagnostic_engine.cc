#include "diagnostic_engine.hh"
#include <algorithm>
#include <cstddef>
#include <sstream>

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
    case DiagnosticError::Category::Driver:
        return "driver error";
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
underlineFor(const location &loc, const std::string &line) {
    const int startColumn = std::max(1, loc.begin.column);
    int width = 1;
    if (loc.begin.line == loc.end.line && loc.end.column > loc.begin.column) {
        width = loc.end.column - loc.begin.column;
    } else if (static_cast<std::size_t>(startColumn) <= line.size()) {
        width = static_cast<int>(line.size() -
                                 static_cast<std::size_t>(startColumn) + 1);
    }
    return std::string(static_cast<std::size_t>(startColumn - 1), ' ') +
           std::string(static_cast<std::size_t>(std::max(1, width)), '^');
}

}  // namespace

std::string
DiagnosticEngine::render(const DiagnosticError &error,
                         const std::string &fallbackPath) const {
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
    const auto path = locationPath(error, fallbackPath);

    if (!path.empty()) {
        out << " --> " << path << ':' << lineNumber << ':' << columnNumber << '\n';
    } else {
        out << " --> " << lineNumber << ':' << columnNumber << '\n';
    }

    const SourceBuffer *source = sources_ ? sources_->find(loc) : nullptr;
    if (!source && !path.empty() && sources_) {
        source = sources_->find(path);
    }
    if (source) {
        if (const auto *sourceLine =
                source->line(static_cast<std::size_t>(lineNumber))) {
            const auto lineLabel = std::to_string(lineNumber);
            out << ' ' << std::string(lineLabel.size(), ' ') << " |\n";
            out << ' ' << lineLabel << " | " << *sourceLine << '\n';
            out << ' ' << std::string(lineLabel.size(), ' ') << " | "
                << underlineFor(loc, *sourceLine) << '\n';
        }
    }

    if (!error.hint().empty()) {
        out << "help: " << error.hint() << '\n';
    }
    return out.str();
}

void
DiagnosticEngine::emit(const DiagnosticError &error, std::ostream &out,
                       const std::string &fallbackPath) const {
    out << render(error, fallbackPath);
}

}  // namespace lona
