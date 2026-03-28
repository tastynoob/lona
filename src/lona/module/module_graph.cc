#include "module_graph.hh"
#include "lona/err/err.hh"
#include "lona/source/source_manager.hh"
#include <algorithm>
#include <unordered_set>
#include <utility>

namespace lona {

const std::vector<string> kModuleGraphEmptyDependencies;

void
collectModuleGraphPostOrder(const ModuleGraph &moduleGraph,
                            const string &path,
                            std::unordered_set<string> &visited,
                            std::vector<string> &ordered) {
    if (!visited.emplace(path).second) {
        return;
    }
    for (const auto &dependency : moduleGraph.dependenciesOf(path)) {
        collectModuleGraphPostOrder(moduleGraph, dependency, visited, ordered);
    }
    ordered.push_back(path);
}

ModuleGraph::ModuleRecord &
ModuleGraph::requireRecord(const string &path) {
    auto found = records_.find(path);
    if (found == records_.end()) {
        throw std::runtime_error("module record not found: " + toStdString(path));
    }
    return *found->second;
}

const ModuleGraph::ModuleRecord &
ModuleGraph::requireRecord(const string &path) const {
    auto found = records_.find(path);
    if (found == records_.end()) {
        throw std::runtime_error("module record not found: " + toStdString(path));
    }
    return *found->second;
}

CompilationUnit &
ModuleGraph::getOrCreate(const SourceBuffer &source) {
    auto inserted = records_.emplace(
        source.path(),
        std::make_unique<ModuleRecord>(
            std::make_unique<CompilationUnit>(source)));
    if (inserted.second) {
        const auto &moduleName = inserted.first->second->unit->moduleName();
        moduleNameToPath_[moduleName] = source.path();
        loadOrder_.push_back(source.path());
    } else {
        inserted.first->second->unit->refreshSource(source);
    }
    return *inserted.first->second->unit;
}

CompilationUnit *
ModuleGraph::find(const string &path) {
    auto found = records_.find(path);
    if (found == records_.end()) {
        return nullptr;
    }
    return found->second->unit.get();
}

CompilationUnit *
ModuleGraph::findByModuleName(const string &moduleName) {
    auto found = moduleNameToPath_.find(moduleName);
    if (found == moduleNameToPath_.end()) {
        return nullptr;
    }
    return find(found->second);
}

const CompilationUnit *
ModuleGraph::find(const string &path) const {
    auto found = records_.find(path);
    if (found == records_.end()) {
        return nullptr;
    }
    return found->second->unit.get();
}

const CompilationUnit *
ModuleGraph::findByModuleName(const string &moduleName) const {
    auto found = moduleNameToPath_.find(moduleName);
    if (found == moduleNameToPath_.end()) {
        return nullptr;
    }
    return find(found->second);
}

void
ModuleGraph::markRoot(const string &path) {
    if (!rootPath_.empty()) {
        requireRecord(rootPath_).root = false;
    }
    auto &record = requireRecord(path);
    record.root = true;
    rootPath_ = path;
}

CompilationUnit *
ModuleGraph::root() {
    if (rootPath_.empty()) {
        return nullptr;
    }
    return requireRecord(rootPath_).unit.get();
}

const CompilationUnit *
ModuleGraph::root() const {
    if (rootPath_.empty()) {
        return nullptr;
    }
    return requireRecord(rootPath_).unit.get();
}

void
ModuleGraph::resetDependencies(const string &path) {
    auto &dependencies = requireRecord(path).dependencies;
    for (const auto &dependency : dependencies) {
        auto reverse = reverseDependencies_.find(dependency);
        if (reverse == reverseDependencies_.end()) {
            continue;
        }
        auto &dependents = reverse->second;
        dependents.erase(
            std::remove(dependents.begin(), dependents.end(), path),
            dependents.end());
        if (dependents.empty()) {
            reverseDependencies_.erase(reverse);
        }
    }
    dependencies.clear();
}

void
ModuleGraph::addDependency(const string &path, string dependencyPath) {
    auto &dependencies = requireRecord(path).dependencies;
    if (std::find(dependencies.begin(), dependencies.end(), dependencyPath) ==
        dependencies.end()) {
        reverseDependencies_[dependencyPath].push_back(path);
        dependencies.push_back(std::move(dependencyPath));
    }
}

const std::vector<string> &
ModuleGraph::dependenciesOf(const string &path) const {
    auto found = records_.find(path);
    if (found == records_.end()) {
        return kModuleGraphEmptyDependencies;
    }
    return found->second->dependencies;
}

const std::vector<string> &
ModuleGraph::dependentsOf(const string &path) const {
    auto found = reverseDependencies_.find(path);
    if (found == reverseDependencies_.end()) {
        return kModuleGraphEmptyDependencies;
    }
    return found->second;
}

std::vector<string>
ModuleGraph::postOrderFrom(const string &path) const {
    if (find(path) == nullptr) {
        return {};
    }

    std::unordered_set<string> visited;
    std::vector<string> ordered;
    collectModuleGraphPostOrder(*this, path, visited, ordered);
    return ordered;
}

}  // namespace lona
