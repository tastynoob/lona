#include "cmdline.hpp"
#include "scan/driver.hh"
#include "visitor.hh"
#include <fstream>

int
main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <input-file>" << std::endl;
        return 1;
    }
    std::string filepath = argv[1];
    std::ifstream in(filepath);
    if (in.fail()) {
        std::cerr << "Failed to open file: " << filepath << std::endl;
        return 1;
    }
    std::string filename;
    if (filepath.rfind('/') != std::string::npos) {
        filename = filepath.substr(filepath.rfind('/') + 1);
    } else {
        filename = filepath;
    }

    lona::Driver driver;
    driver.input(&in);
    auto tree = driver.parse();
    if (tree) {
        Json json;
        tree->toJson(json);
        std::ofstream out("ast.json");
        out << json.dump(4) << std::endl;
        out.close();

        std::ofstream out2("ir.ll");
        lona::compile(tree, filename, out2);
    }

    return 0;
}