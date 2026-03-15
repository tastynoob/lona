#include "module_graph.hh"
#include "lona/err/err.hh"
#include "lona/source/source_manager.hh"
#include <algorithm>
#include <utility>

namespace lona {
namespace {

const std::vector<std::string> kEmptyDependencies;

}

ModuleGraph::ModuleRecord &
ModuleGraph::requireRecord(const std::string &path) {
    auto found = records_.find(path);
    if (found == records_.end()) {
        throw std::runtime_error("module record not found: " + path);
    }
    return *found->second;
}

const ModuleGraph::ModuleRecord &
ModuleGraph::requireRecord(const std::string &path) const {
    auto found = records_.find(path);
    if (found == records_.end()) {
        throw std::runtime_error("module record not found: " + path);
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
        auto existing = moduleNameToPath_.find(moduleName);
        if (existing != moduleNameToPath_.end() && existing->second != source.path()) {
            throw DiagnosticError(
                DiagnosticError::Category::Semantic,
                "module name `" + moduleName + "` is ambiguous between `" +
                    existing->second + "` and `" + source.path() + "`.",
                "Rename one of the files so imported modules have distinct base names.");
        }
        moduleNameToPath_[moduleName] = source.path();
        loadOrder_.push_back(source.path());
    } else {
        inserted.first->second->unit->refreshSource(source);
    }
    return *inserted.first->second->unit;
}

CompilationUnit *
ModuleGraph::find(const std::string &path) {
    auto found = records_.find(path);
    if (found == records_.end()) {
        return nullptr;
    }
    return found->second->unit.get();
}

CompilationUnit *
ModuleGraph::findByModuleName(const std::string &moduleName) {
    auto found = moduleNameToPath_.find(moduleName);
    if (found == moduleNameToPath_.end()) {
        return nullptr;
    }
    return find(found->second);
}

const CompilationUnit *
ModuleGraph::find(const std::string &path) const {
    auto found = records_.find(path);
    if (found == records_.end()) {
        return nullptr;
    }
    return found->second->unit.get();
}

const CompilationUnit *
ModuleGraph::findByModuleName(const std::string &moduleName) const {
    auto found = moduleNameToPath_.find(moduleName);
    if (found == moduleNameToPath_.end()) {
        return nullptr;
    }
    return find(found->second);
}

void
ModuleGraph::markRoot(const std::string &path) {
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
ModuleGraph::addDependency(const std::string &path, std::string dependencyPath) {
    auto &dependencies = requireRecord(path).dependencies;
    if (std::find(dependencies.begin(), dependencies.end(), dependencyPath) ==
        dependencies.end()) {
        dependencies.push_back(std::move(dependencyPath));
    }
}

const std::vector<std::string> &
ModuleGraph::dependenciesOf(const std::string &path) const {
    auto found = records_.find(path);
    if (found == records_.end()) {
        return kEmptyDependencies;
    }
    return found->second->dependencies;
}

}  // namespace lona
