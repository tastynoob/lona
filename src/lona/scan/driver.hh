#include "parser.hh"
#include "lona/diag/diagnostic_bag.hh"
#include "scanner.hh"
#include <string>

namespace lona {

class SourceBuffer;

class Driver {
    friend Parser;
    Scanner *scanner = nullptr;  // lexer
    Parser *parser = nullptr;    // parser

    AstNode *tree = nullptr;  // finally astTree
    const SourceBuffer *source = nullptr;
    DiagnosticBag *diagnostics_ = nullptr;

public:
    Driver();

    void input(std::istream *in, const SourceBuffer &source);
    void setDiagnosticBag(DiagnosticBag *diagnostics) {
        diagnostics_ = diagnostics;
    }

    // return current tokenid
    int token(Parser::semantic_type *const lval, Parser::location_type *loc);

    // parse return astTree
    AstNode *parse();
    void reportSyntaxError(const Parser::location_type &loc,
                           const std::string &rawMessage);
};

}  // namespace lona
