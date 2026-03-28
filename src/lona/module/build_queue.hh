#pragma once

#include "module_graph.hh"
#include <deque>
#include <string>

namespace lona {

class ModuleBuildQueue {
    std::deque<string> pending_;

public:
    void reset(const ModuleGraph &moduleGraph, const string &rootPath);
    void reset(const ModuleGraph &moduleGraph, const std::string &rootPath) {
        reset(moduleGraph, string(rootPath));
    }
    bool empty() const { return pending_.empty(); }
    string popNext();
};

}  // namespace lona
