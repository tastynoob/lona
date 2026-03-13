#include "parser.hh"
#include "scanner.hh"
#include <string>

namespace lona {

class Driver {
    friend Parser;
    Scanner *scanner = nullptr;  // lexer
    Parser *parser = nullptr;    // parser

    AstNode *tree;  // finally astTree
    std::string sourcePath;
    const std::string *stablePath = nullptr;
public:
    Driver();

    void input(std::istream *in, const std::string &path = "");

    // return current tokenid
    int token(Parser::semantic_type *const lval, Parser::location_type *loc);

    // parse return astTree
    AstNode *parse();
};

}  // namespace lona