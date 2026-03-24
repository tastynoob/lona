#include "module_artifact.hh"
#include <utility>

namespace lona {

ModuleArtifact::ModuleArtifact(std::string path, std::string moduleKey,
                               std::string moduleName, std::uint64_t sourceHash,
                               std::uint64_t interfaceHash,
                               std::uint64_t implementationHash)
    : path_(std::move(path)),
      moduleKey_(std::move(moduleKey)),
      moduleName_(std::move(moduleName)),
      sourceHash_(sourceHash),
      interfaceHash_(interfaceHash),
      implementationHash_(implementationHash) {}

void
ModuleArtifact::setDependencyInterfaceHashes(
    std::unordered_map<std::string, std::uint64_t> dependencyInterfaceHashes) {
    dependencyInterfaceHashes_ = std::move(dependencyInterfaceHashes);
}

void
ModuleArtifact::setCompileProfile(std::string targetTriple, int optLevel,
                                  bool debugInfo) {
    targetTriple_ = std::move(targetTriple);
    optLevel_ = optLevel;
    debugInfo_ = debugInfo;
}

void
ModuleArtifact::setBitcode(ByteBuffer bitcode) {
    bitcode_ = std::move(bitcode);
}

void
ModuleArtifact::setObjectCode(ByteBuffer objectCode) {
    objectCode_ = std::move(objectCode);
}

void
ModuleArtifact::setContainsNativeAbi(bool containsNativeAbi) {
    containsNativeAbi_ = containsNativeAbi;
}

}  // namespace lona
