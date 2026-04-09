#include "lona/scan/scanner.hh"
#include <algorithm>
#include <cstring>

namespace lona {

namespace {

constexpr int kScannerBufferSize = 256 * 1024;

}  // namespace

Scanner::Scanner(std::istream *in, std::string_view input)
    : yyFlexLexer(in), input_(input) {
    if (in != nullptr) {
        yy_switch_to_buffer(yy_create_buffer(in, kScannerBufferSize));
    }
}

int
Scanner::LexerInput(char *buf, int max_size) {
    if (buf == nullptr || max_size <= 0 || inputOffset_ >= input_.size()) {
        return 0;
    }

    auto remaining = input_.size() - inputOffset_;
    auto count = std::min<std::size_t>(remaining,
                                       static_cast<std::size_t>(max_size));
    std::memcpy(buf, input_.data() + inputOffset_, count);
    inputOffset_ += count;
    return static_cast<int>(count);
}

}  // namespace lona
