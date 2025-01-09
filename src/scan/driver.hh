#include "parser.hh"
#include "scan/scanner.hh"

namespace lona {

class Driver {
    friend Parser;
    Scanner *scanner = nullptr;  // lexer
    Parser *parser = nullptr;    // parser

    AstNode *tree;  // finally astTree
public:
    Driver();

    void input(std::istream *in);

    // return current tokenid
    int token(Parser::semantic_type *const lval, Parser::location_type *loc);

    // parse return astTree
    AstNode *parse();
};

}  // namespace lona