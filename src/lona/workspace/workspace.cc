#include "workspace.hh"
#include <utility>

namespace lona {

CompilerWorkspace::CompilerWorkspace()
    : diagnostics_(&sourceManager_) {}

const SourceBuffer &
CompilerWorkspace::loadSource(const std::string &path) {
    return sourceManager_.loadFile(path);
}

CompilationUnit &
CompilerWorkspace::loadUnit(const std::string &path) {
    const auto &source = loadSource(path);
    auto &unit = moduleGraph_.getOrCreate(source);
    unit.attachInterface(
        moduleCache_.getOrCreate(source, unit.moduleKey(), unit.moduleName()));
    return unit;
}

CompilationUnit &
CompilerWorkspace::loadRootUnit(const std::string &path) {
    auto &unit = loadUnit(path);
    moduleGraph_.markRoot(unit.path());
    return unit;
}

ModuleArtifact *
CompilerWorkspace::findArtifact(const std::string &path) {
    auto found = moduleArtifacts_.find(path);
    if (found == moduleArtifacts_.end()) {
        return nullptr;
    }
    return &found->second;
}

const ModuleArtifact *
CompilerWorkspace::findArtifact(const std::string &path) const {
    auto found = moduleArtifacts_.find(path);
    if (found == moduleArtifacts_.end()) {
        return nullptr;
    }
    return &found->second;
}

void
CompilerWorkspace::storeArtifact(ModuleArtifact artifact) {
    moduleArtifacts_[artifact.path()] = std::move(artifact);
}

}  // namespace lona
