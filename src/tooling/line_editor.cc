#include "tooling/line_editor.hh"
#include <cerrno>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace lona::tooling {

namespace {

constexpr int kInputFd = STDIN_FILENO;
constexpr int kOutputFd = STDOUT_FILENO;

bool
writeAll(std::string_view text) {
    while (!text.empty()) {
        auto written = ::write(kOutputFd, text.data(), text.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

struct RawModeGuard {
    termios original {};
    bool enabled = false;

    RawModeGuard() {
        if (!::isatty(kInputFd) || !::isatty(kOutputFd)) {
            return;
        }
        if (::tcgetattr(kInputFd, &original) != 0) {
            return;
        }

        auto raw = original;
        raw.c_iflag &= static_cast<unsigned long>(~(BRKINT | ICRNL | INPCK |
                                                    ISTRIP | IXON));
        raw.c_oflag &= static_cast<unsigned long>(~OPOST);
        raw.c_cflag |= CS8;
        raw.c_lflag &= static_cast<unsigned long>(~(ECHO | ICANON | IEXTEN));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(kInputFd, TCSAFLUSH, &raw) != 0) {
            return;
        }
        enabled = true;
    }

    ~RawModeGuard() {
        if (enabled) {
            (void)::tcsetattr(kInputFd, TCSAFLUSH, &original);
        }
    }
};

enum class Key {
    Unknown,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    Delete,
};

std::optional<char>
readByte() {
    char ch = '\0';
    while (true) {
        auto count = ::read(kInputFd, &ch, 1);
        if (count == 1) {
            return ch;
        }
        if (errno == EINTR) {
            continue;
        }
        return std::nullopt;
    }
}

std::optional<char>
readByteWithTimeout(int timeoutMs) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(kInputFd, &readSet);

    timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    auto ready =
        ::select(kInputFd + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return std::nullopt;
    }
    return readByte();
}

Key
readEscapeKey() {
    auto first = readByteWithTimeout(100);
    if (!first.has_value()) {
        return Key::Unknown;
    }
    if (*first == '[') {
        auto second = readByteWithTimeout(100);
        if (!second.has_value()) {
            return Key::Unknown;
        }
        if (*second >= '0' && *second <= '9') {
            std::string digits(1, *second);
            while (true) {
                auto next = readByteWithTimeout(100);
                if (!next.has_value()) {
                    return Key::Unknown;
                }
                if (*next == '~') {
                    if (digits == "1" || digits == "7") {
                        return Key::Home;
                    }
                    if (digits == "3") {
                        return Key::Delete;
                    }
                    if (digits == "4" || digits == "8") {
                        return Key::End;
                    }
                    return Key::Unknown;
                }
                if (*next >= '0' && *next <= '9') {
                    digits.push_back(*next);
                    continue;
                }
                if (*next == ';') {
                    while (true) {
                        auto modifier = readByteWithTimeout(100);
                        if (!modifier.has_value()) {
                            return Key::Unknown;
                        }
                        if (*modifier >= 'A' && *modifier <= 'Z') {
                            switch (*modifier) {
                                case 'A':
                                    return Key::Up;
                                case 'B':
                                    return Key::Down;
                                case 'C':
                                    return Key::Right;
                                case 'D':
                                    return Key::Left;
                                case 'F':
                                    return Key::End;
                                case 'H':
                                    return Key::Home;
                                default:
                                    return Key::Unknown;
                            }
                        }
                    }
                }
                return Key::Unknown;
            }
        }
        switch (*second) {
            case 'A':
                return Key::Up;
            case 'B':
                return Key::Down;
            case 'C':
                return Key::Right;
            case 'D':
                return Key::Left;
            case 'F':
                return Key::End;
            case 'H':
                return Key::Home;
            default:
                return Key::Unknown;
        }
    }
    if (*first == 'O') {
        auto second = readByteWithTimeout(100);
        if (!second.has_value()) {
            return Key::Unknown;
        }
        switch (*second) {
            case 'F':
                return Key::End;
            case 'H':
                return Key::Home;
            default:
                return Key::Unknown;
        }
    }
    return Key::Unknown;
}

}  // namespace

LineEditor::LineEditor(std::string_view prompt) : prompt_(prompt) {}

bool
LineEditor::supported() {
    if (!::isatty(kInputFd) || !::isatty(kOutputFd)) {
        return false;
    }
    termios state {};
    return ::tcgetattr(kInputFd, &state) == 0;
}

void
LineEditor::refreshLine(std::string_view line, std::size_t cursor) const {
    std::string output;
    output.reserve(prompt_.size() * 2 + line.size() + 32);
    output += '\r';
    output += prompt_;
    output.append(line.data(), line.size());
    output += "\x1b[K";
    const auto cursorColumns = prompt_.size() + cursor;
    if (cursorColumns > 0) {
        output += '\r';
        output += "\x1b[";
        output += std::to_string(cursorColumns);
        output += 'C';
    }
    (void)writeAll(output);
}

void
LineEditor::appendHistory(std::string line) {
    if (line.empty()) {
        return;
    }
    if (!history_.empty() && history_.back() == line) {
        return;
    }
    history_.push_back(std::move(line));
}

std::optional<std::string>
LineEditor::readLine() {
    std::cout.flush();
    RawModeGuard rawMode;
    if (!rawMode.enabled) {
        return std::nullopt;
    }

    std::string line;
    std::size_t cursor = 0;
    historyIndex_.reset();
    historyDraft_.clear();
    refreshLine(line, cursor);

    while (true) {
        auto next = readByte();
        if (!next.has_value()) {
            if (line.empty()) {
                (void)writeAll("\r\n");
                return std::nullopt;
            }
            continue;
        }

        const unsigned char ch = static_cast<unsigned char>(*next);
        if (ch == '\r' || ch == '\n') {
            (void)writeAll("\r\n");
            appendHistory(line);
            historyIndex_.reset();
            historyDraft_.clear();
            return line;
        }
        if (ch == 4) {
            if (line.empty()) {
                (void)writeAll("\r\n");
                return std::nullopt;
            }
            if (cursor < line.size()) {
                line.erase(cursor, 1);
                refreshLine(line, cursor);
            }
            continue;
        }
        if (ch == 1) {
            cursor = 0;
            refreshLine(line, cursor);
            continue;
        }
        if (ch == 5) {
            cursor = line.size();
            refreshLine(line, cursor);
            continue;
        }
        if (ch == 2) {
            if (cursor > 0) {
                --cursor;
                refreshLine(line, cursor);
            }
            continue;
        }
        if (ch == 6) {
            if (cursor < line.size()) {
                ++cursor;
                refreshLine(line, cursor);
            }
            continue;
        }
        if (ch == 8 || ch == 127) {
            if (cursor > 0) {
                line.erase(cursor - 1, 1);
                --cursor;
                refreshLine(line, cursor);
            }
            continue;
        }
        if (ch == 27) {
            switch (readEscapeKey()) {
                case Key::Left:
                    if (cursor > 0) {
                        --cursor;
                        refreshLine(line, cursor);
                    }
                    break;
                case Key::Right:
                    if (cursor < line.size()) {
                        ++cursor;
                        refreshLine(line, cursor);
                    }
                    break;
                case Key::Home:
                    cursor = 0;
                    refreshLine(line, cursor);
                    break;
                case Key::End:
                    cursor = line.size();
                    refreshLine(line, cursor);
                    break;
                case Key::Delete:
                    if (cursor < line.size()) {
                        line.erase(cursor, 1);
                        refreshLine(line, cursor);
                    }
                    break;
                case Key::Up:
                    if (!history_.empty()) {
                        if (!historyIndex_.has_value()) {
                            historyDraft_ = line;
                            historyIndex_ = history_.size() - 1;
                        } else if (*historyIndex_ > 0) {
                            --(*historyIndex_);
                        }
                        line = history_[*historyIndex_];
                        cursor = line.size();
                        refreshLine(line, cursor);
                    }
                    break;
                case Key::Down:
                    if (historyIndex_.has_value()) {
                        if (*historyIndex_ + 1 < history_.size()) {
                            ++(*historyIndex_);
                            line = history_[*historyIndex_];
                        } else {
                            historyIndex_.reset();
                            line = historyDraft_;
                        }
                        cursor = line.size();
                        refreshLine(line, cursor);
                    }
                    break;
                case Key::Unknown:
                    break;
            }
            continue;
        }
        if (ch >= 32 && ch < 127) {
            line.insert(cursor, 1, static_cast<char>(ch));
            ++cursor;
            refreshLine(line, cursor);
        }
    }
}

}  // namespace lona::tooling
