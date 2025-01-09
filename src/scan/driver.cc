#include "scan/driver.hh"

namespace lona {

Driver::Driver() { parser = new Parser(*this); }

void
Driver::input(std::istream *in) {
    if (scanner) delete scanner;
    scanner = new Scanner(in);
}

int
Driver::token(Parser::semantic_type *const lval, Parser::location_type *loc) {
    int id = scanner->yylex(lval, loc);
    return id;
}

AstNode *
Driver::parse() {
    if (parser->parse() == 0) return tree;
    return nullptr;
}

}  // namespace lona
