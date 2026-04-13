#pragma once

#include "lona/err/err.hh"
#include <cstddef>
#include <exception>
#include <utility>
#include <vector>

namespace lona {

class DiagnosticLimitReached : public std::exception {
    std::size_t limit_ = 0;

public:
    explicit DiagnosticLimitReached(std::size_t limit) : limit_(limit) {}

    std::size_t limit() const { return limit_; }

    const char *what() const noexcept override {
        return "diagnostic limit reached";
    }
};

class DiagnosticBag {
    std::vector<DiagnosticError> diagnostics_;
    std::size_t maxErrors_ = 20;
    bool truncated_ = false;

public:
    explicit DiagnosticBag(std::size_t maxErrors = 20)
        : maxErrors_(maxErrors) {}

    void clear() {
        diagnostics_.clear();
        truncated_ = false;
    }

    std::size_t maxErrors() const { return maxErrors_; }
    bool truncated() const { return truncated_; }
    bool hasDiagnostics() const { return !diagnostics_.empty(); }
    std::size_t size() const { return diagnostics_.size(); }

    bool full() const {
        return maxErrors_ != 0 && diagnostics_.size() >= maxErrors_;
    }

    bool hasCategory(DiagnosticError::Category category) const {
        for (const auto &diagnostic : diagnostics_) {
            if (diagnostic.category() == category) {
                return true;
            }
        }
        return false;
    }

    const std::vector<DiagnosticError> &diagnostics() const {
        return diagnostics_;
    }

    bool add(DiagnosticError diagnostic) {
        if (full()) {
            truncated_ = true;
            return false;
        }

        diagnostics_.push_back(std::move(diagnostic));
        if (full()) {
            truncated_ = true;
            return false;
        }
        return true;
    }
};

}  // namespace lona
