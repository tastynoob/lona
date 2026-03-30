#include "workspace.hh"
#include <utility>

namespace lona {

namespace {

string
artifactCacheKey(const string &path, ModuleEntryRole entryRole) {
    return path +
           (entryRole == ModuleEntryRole::Root ? "#root" : "#dependency");
}

}  // namespace

CompilerWorkspace::CompilerWorkspace() : diagnostics_(&sourceManager_) {}

const SourceBuffer &
CompilerWorkspace::loadSource(const string &path) {
    return sourceManager_.loadFile(toStdString(path));
}

CompilationUnit &
CompilerWorkspace::loadUnit(const string &path) {
    const auto &source = loadSource(path);
    auto &unit = moduleGraph_.getOrCreate(source);
    unit.attachInterface(
        moduleCache_.getOrCreate(source, unit.moduleKey(), unit.moduleName()));
    return unit;
}

CompilationUnit &
CompilerWorkspace::loadRootUnit(const string &path) {
    auto &unit = loadUnit(path);
    moduleGraph_.markRoot(unit.path());
    return unit;
}

ModuleArtifact *
CompilerWorkspace::findArtifact(const string &path, ModuleEntryRole entryRole) {
    auto found = moduleArtifacts_.find(artifactCacheKey(path, entryRole));
    if (found == moduleArtifacts_.end()) {
        return nullptr;
    }
    return &found->second;
}

const ModuleArtifact *
CompilerWorkspace::findArtifact(const string &path,
                                ModuleEntryRole entryRole) const {
    auto found = moduleArtifacts_.find(artifactCacheKey(path, entryRole));
    if (found == moduleArtifacts_.end()) {
        return nullptr;
    }
    return &found->second;
}

void
CompilerWorkspace::storeArtifact(ModuleArtifact artifact) {
    moduleArtifacts_[artifactCacheKey(artifact.path(), artifact.entryRole())] =
        std::move(artifact);
}

}  // namespace lona
