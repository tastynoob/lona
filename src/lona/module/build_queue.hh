#pragma once

#include "module_graph.hh"
#include <deque>
#include <string>

namespace lona {

class ModuleBuildQueue {
    std::deque<std::string> pending_;

public:
    void reset(const ModuleGraph &moduleGraph, const std::string &rootPath);
    bool empty() const { return pending_.empty(); }
    std::string popNext();
};

}  // namespace lona
