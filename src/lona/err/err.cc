#include "err.h"
#include "err.hh"
#include <cassert>

namespace lona {

Err::Err(std::ifstream &ifs) {
    while (!ifs.eof()) {
        lines.emplace_back();
        std::getline(ifs, lines.back());
    }
    ifs.clear();
    ifs.seekg(0);
}

void
Err::err(const position &pos, std::string msg) {
    assert(lines.size() >= pos.line);
    // read the file of line this
    std::string errline = lines[pos.line - 1];
    std::cout << errline << std::endl;
    std::cout << std::string(pos.column - 1, ' ') << "^ " << msg << std::endl;
    exit(-1);
}

}
