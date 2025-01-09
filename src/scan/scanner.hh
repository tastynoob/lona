#pragma once

#if !defined(yyFlexLexerOnce)
#include <FlexLexer.h>
#endif

#include "parser.hh"

namespace lona {

class Scanner : public yyFlexLexer {
public:
    Scanner(std::istream *in) : yyFlexLexer(in) {}

    using FlexLexer::yylex;
    virtual int yylex(Parser::semantic_type *const lval,
                      Parser::location_type *loc);
};

}  // namespace lona