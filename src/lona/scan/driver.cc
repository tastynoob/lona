#include "../scan/driver.hh"
#include "lona/source/source_manager.hh"

namespace lona {

Driver::Driver() { parser = new Parser(*this); }

void
Driver::input(std::istream *in, const SourceBuffer &newSource) {
    if (scanner) delete scanner;
    scanner = new Scanner(in);
    source = &newSource;
}

int
Driver::token(Parser::semantic_type *const lval, Parser::location_type *loc) {
    if (loc && source) {
        loc->begin.filename = source->stablePath();
        loc->end.filename = source->stablePath();
    }
    int id = scanner->yylex(lval, loc);
    return id;
}

AstNode *
Driver::parse() {
    if (parser->parse() == 0) return tree;
    return nullptr;
}

}  // namespace lona
