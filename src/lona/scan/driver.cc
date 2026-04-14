#include "../scan/driver.hh"
#include "lona/ast/token.hh"
#include "lona/source/source_manager.hh"

namespace lona {

Driver::Driver() { parser = new Parser(*this); }

Driver::~Driver() {
    clearTrackedTokens();
    delete scanner;
    delete parser;
}

void
Driver::clearTrackedTokens() {
    for (auto *token : trackedTokens_) {
        delete token;
    }
    trackedTokens_.clear();
}

void
Driver::input(std::istream *in, const SourceBuffer &newSource) {
    clearTrackedTokens();
    if (scanner) delete scanner;
    scanner = new Scanner(in, newSource.content());
    source = &newSource;
}

int
Driver::token(Parser::semantic_type *const lval, Parser::location_type *loc) {
    if (loc && source) {
        loc->begin.filename = source->stablePath();
        loc->end.filename = source->stablePath();
    }
    if (lval) {
        lval->token = nullptr;
    }
    int id = scanner->yylex(lval, loc);
    if (lval && lval->token) {
        trackedTokens_.push_back(lval->token);
    }
    return id;
}

AstNode *
Driver::parse() {
    if (parser->parse() == 0) return tree;
    return nullptr;
}

void
Driver::reportSyntaxError(const Parser::location_type &loc,
                          const std::string &rawMessage) {
    DiagnosticError diagnostic(
        DiagnosticError::Category::Syntax, loc,
        friendlySyntaxMessage(rawMessage),
        "Check for a missing separator, unmatched delimiter, or mistyped "
        "keyword near here.");
    if (!diagnostics_) {
        throw diagnostic;
    }
    if (!diagnostics_->add(std::move(diagnostic))) {
        throw DiagnosticLimitReached(diagnostics_->maxErrors());
    }
}

}  // namespace lona
