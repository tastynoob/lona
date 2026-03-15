#include "source_manager.hh"
#include "lona/err/err.hh"
#include <fstream>
#include <sstream>
#include <utility>

namespace lona {
namespace {

std::vector<std::string>
splitLines(const std::string &content) {
    std::vector<std::string> lines;
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    if (lines.empty() && !content.empty()) {
        lines.push_back(content);
    }
    return lines;
}

}  // namespace

SourceBuffer::SourceBuffer(std::string path, std::string content)
    : path_(std::move(path)) {
    resetContent(std::move(content));
}

const std::string *
SourceBuffer::line(std::size_t lineNumber) const {
    if (lineNumber == 0 || lineNumber > lines_.size()) {
        return nullptr;
    }
    return &lines_[lineNumber - 1];
}

void
SourceBuffer::resetContent(std::string content) {
    content_ = std::move(content);
    lines_ = splitLines(content_);
}

const SourceBuffer &
SourceManager::loadFile(const std::string &path) {
    if (const auto *existing = find(path)) {
        return *existing;
    }

    std::ifstream input(path);
    if (!input) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't open input file `" + path + "`.",
            "Check that the path exists and that you have read permission.");
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "I couldn't read input file `" + path + "`.",
            "Check that the file is readable and not truncated.");
    }

    return addSource(path, buffer.str());
}

const SourceBuffer &
SourceManager::addSource(std::string path, std::string content) {
    auto inserted = sources_.emplace(
        path, std::make_unique<SourceBuffer>(path, std::move(content)));
    if (!inserted.second) {
        inserted.first->second->resetContent(std::move(content));
    }
    return *inserted.first->second;
}

const SourceBuffer *
SourceManager::find(const std::string &path) const {
    auto found = sources_.find(path);
    if (found == sources_.end()) {
        return nullptr;
    }
    return found->second.get();
}

const SourceBuffer *
SourceManager::find(const location &loc) const {
    if (!loc.begin.filename) {
        return nullptr;
    }
    return find(*loc.begin.filename);
}

}  // namespace lona
