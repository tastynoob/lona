#include <iostream>

#include "cfg.hh"
#include "../ast/astnode.hh"

namespace lona {

bool
CFGChecker::hasNoReturnedNode() {
    for (auto it : endNodes) {
        if (!it->is<AstRet>()) {
            return true;
        }
    }
    return false;
}

void
CFGChecker::gen(std::ostream &os) {
    os << "digraph AST {\n";
    os << this->strbuf.str();
    os << "}\n";
}

}
