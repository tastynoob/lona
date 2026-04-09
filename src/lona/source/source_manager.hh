#pragma once

#include "location.hh"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lona {

class SourceBuffer {
    std::string path_;
    std::string content_;
    std::vector<std::size_t> lineOffsets_;

public:
    SourceBuffer(std::string path, std::string content);

    const std::string &path() const { return path_; }
    const std::string *stablePath() const { return &path_; }
    const std::string &content() const { return content_; }
    std::size_t lineCount() const { return lineOffsets_.size(); }
    std::optional<std::string_view> line(std::size_t lineNumber) const;
    void resetContent(std::string content);
};

class SourceManager {
    std::unordered_map<std::string, std::unique_ptr<SourceBuffer>> sources_;

public:
    const SourceBuffer &loadFile(const std::string &path);
    const SourceBuffer &addSource(std::string path, std::string content);

    const SourceBuffer *find(const std::string &path) const;
    const SourceBuffer *find(const location &loc) const;
};

}  // namespace lona
