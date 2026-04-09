#pragma once

#if !defined(yyFlexLexerOnce)
#include <FlexLexer.h>
#endif

#include "parser.hh"
#include <cstddef>
#include <string_view>

namespace lona {

class Scanner : public yyFlexLexer {
    std::string_view input_;
    std::size_t inputOffset_ = 0;

public:
    Scanner(std::istream *in, std::string_view input);

    using FlexLexer::yylex;
    virtual int yylex(Parser::semantic_type *const lval,
                      Parser::location_type *loc);

protected:
    int LexerInput(char *buf, int max_size) override;
};

}  // namespace lona
