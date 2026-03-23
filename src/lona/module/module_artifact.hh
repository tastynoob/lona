#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace lona {

class ModuleArtifact {
    std::string path_;
    std::string moduleKey_;
    std::string moduleName_;
    std::uint64_t sourceHash_ = 0;
    std::uint64_t interfaceHash_ = 0;
    std::uint64_t implementationHash_ = 0;
    std::unordered_map<std::string, std::uint64_t> dependencyInterfaceHashes_;
    std::string targetTriple_;
    int optLevel_ = 0;
    bool debugInfo_ = false;
    std::string llvmIR_;

public:
    ModuleArtifact() = default;
    ModuleArtifact(std::string path, std::string moduleKey, std::string moduleName,
                   std::uint64_t sourceHash, std::uint64_t interfaceHash,
                   std::uint64_t implementationHash);

    const std::string &path() const { return path_; }
    const std::string &moduleKey() const { return moduleKey_; }
    const std::string &moduleName() const { return moduleName_; }
    std::uint64_t sourceHash() const { return sourceHash_; }
    std::uint64_t interfaceHash() const { return interfaceHash_; }
    std::uint64_t implementationHash() const { return implementationHash_; }
    const std::unordered_map<std::string, std::uint64_t> &dependencyInterfaceHashes()
        const {
        return dependencyInterfaceHashes_;
    }
    const std::string &targetTriple() const { return targetTriple_; }
    int optLevel() const { return optLevel_; }
    bool debugInfo() const { return debugInfo_; }
    const std::string &llvmIR() const { return llvmIR_; }

    void setDependencyInterfaceHashes(
        std::unordered_map<std::string, std::uint64_t> dependencyInterfaceHashes);
    void setCompileProfile(std::string targetTriple, int optLevel, bool debugInfo);
    void setLLVMIR(std::string llvmIR);
};

}  // namespace lona
