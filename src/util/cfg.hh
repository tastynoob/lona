#pragma once
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

namespace lona {
class AstNode;

class CFGChecker {
    uint32_t counter = 0;
    std::stringstream strbuf;
    std::map<AstNode*, bool> footprint;
    std::map<AstNode*, std::string> nodeName;

    std::vector<AstNode*> endNodes;

public:
    std::string nextName(std::string t = "n") {
        std::string name = t + std::to_string(counter);
        counter++;
        return name;
    }

    void visit(AstNode* node) { footprint[node] = true; }

    void visit(AstNode* node, std::string name) {
        assert(nodeName.find(node) == nodeName.end());
        footprint[node] = true;
        nodeName[node] = name;
    }

    bool isVisited(AstNode* node) {
        return footprint.find(node) != footprint.end();
    }

    void link(AstNode* from, AstNode* to) {
        assert(isVisited(from));
        assert(isVisited(to));
        strbuf << nodeName[from] << " -> " << nodeName[to] << ";" << std::endl;
    }

    void addEndNode(AstNode* node) { endNodes.push_back(node); }

    bool multiReturnPath() { return endNodes.size() > 1; }

    // only when has returned value
    bool hasNoReturnedNode();

    void gen(std::ostream& os);
};

}