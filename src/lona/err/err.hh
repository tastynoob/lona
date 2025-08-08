#include "parser.hh"
#include <fstream>
#include <iostream>

namespace lona {

class Err {
    std::vector<std::string> lines;

public:
    Err(std::ifstream &ifs);

    void err(const position &pos, std::string msg);
};

}
