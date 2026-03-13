#include "../scan/driver.hh"
#include <unordered_set>

namespace lona {
namespace {

const std::string *
stableSourcePath(const std::string &path) {
    static std::unordered_set<std::string> paths;
    return &*paths.emplace(path).first;
}

}  // namespace

Driver::Driver() { parser = new Parser(*this); }

void
Driver::input(std::istream *in, const std::string &path) {
    if (scanner) delete scanner;
    scanner = new Scanner(in);
    sourcePath = path;
    stablePath = sourcePath.empty() ? nullptr : stableSourcePath(sourcePath);
}

int
Driver::token(Parser::semantic_type *const lval, Parser::location_type *loc) {
    if (loc && stablePath) {
        loc->begin.filename = stablePath;
        loc->end.filename = stablePath;
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
