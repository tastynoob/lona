#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lona::tooling {

class LineEditor {
    std::string prompt_;
    std::vector<std::string> history_;
    std::optional<std::size_t> historyIndex_;
    std::string historyDraft_;

    void refreshLine(std::string_view line, std::size_t cursor) const;
    void appendHistory(std::string line);

public:
    explicit LineEditor(std::string_view prompt);

    static bool supported();

    std::optional<std::string> readLine();
};

}  // namespace lona::tooling
