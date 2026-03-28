#include "build_queue.hh"

namespace lona {

void
ModuleBuildQueue::reset(const ModuleGraph &moduleGraph,
                        const string &rootPath) {
    pending_.clear();
    for (const auto &path : moduleGraph.postOrderFrom(rootPath)) {
        pending_.push_back(path);
    }
}

string
ModuleBuildQueue::popNext() {
    auto next = pending_.front();
    pending_.pop_front();
    return next;
}

}  // namespace lona
