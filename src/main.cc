#include "cmdline.hpp"
#include "lona/scan/driver.hh"
#include <fstream>

namespace lona {
extern void
compile(std::string &filepath, std::ostream &os);
}



int
main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <input-file>" << std::endl;
        return 1;
    }
    std::string filepath = argv[1];

    std::ofstream out2("ir.ll");
    // lona::compile(filepath, out2);

    return 0;
}