#include "parser.hh"
#include "scanner.hh"
#include <string>

namespace lona {

class SourceBuffer;

class Driver {
    friend Parser;
    Scanner *scanner = nullptr;  // lexer
    Parser *parser = nullptr;    // parser

    AstNode *tree;  // finally astTree
    const SourceBuffer *source = nullptr;

public:
    Driver();

    void input(std::istream *in, const SourceBuffer &source);

    // return current tokenid
    int token(Parser::semantic_type *const lval, Parser::location_type *loc);

    // parse return astTree
    AstNode *parse();
};

}  // namespace lona
