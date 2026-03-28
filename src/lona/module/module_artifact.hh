#pragma once

#include "lona/util/string.hh"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lona {

class ModuleArtifact {
public:
    using ByteBuffer = std::vector<std::uint8_t>;

private:
    string path_;
    string moduleKey_;
    string moduleName_;
    std::uint64_t sourceHash_ = 0;
    std::uint64_t interfaceHash_ = 0;
    std::uint64_t implementationHash_ = 0;
    std::unordered_map<string, std::uint64_t> dependencyInterfaceHashes_;
    string targetTriple_;
    int optLevel_ = 0;
    bool debugInfo_ = false;
    ByteBuffer bitcode_;
    ByteBuffer objectCode_;
    bool containsNativeAbi_ = false;

public:
    ModuleArtifact() = default;
    ModuleArtifact(string path, string moduleKey, string moduleName,
                   std::uint64_t sourceHash, std::uint64_t interfaceHash,
                   std::uint64_t implementationHash);
    ModuleArtifact(std::string path, std::string moduleKey, std::string moduleName,
                   std::uint64_t sourceHash, std::uint64_t interfaceHash,
                   std::uint64_t implementationHash)
        : ModuleArtifact(string(std::move(path)),
                         string(std::move(moduleKey)),
                         string(std::move(moduleName)),
                         sourceHash, interfaceHash, implementationHash) {}

    const string &path() const { return path_; }
    const string &moduleKey() const { return moduleKey_; }
    const string &moduleName() const { return moduleName_; }
    std::uint64_t sourceHash() const { return sourceHash_; }
    std::uint64_t interfaceHash() const { return interfaceHash_; }
    std::uint64_t implementationHash() const { return implementationHash_; }
    const std::unordered_map<string, std::uint64_t> &dependencyInterfaceHashes()
        const {
        return dependencyInterfaceHashes_;
    }
    const string &targetTriple() const { return targetTriple_; }
    int optLevel() const { return optLevel_; }
    bool debugInfo() const { return debugInfo_; }
    const ByteBuffer &bitcode() const { return bitcode_; }
    bool hasBitcode() const { return !bitcode_.empty(); }
    const ByteBuffer &objectCode() const { return objectCode_; }
    bool hasObjectCode() const { return !objectCode_.empty(); }
    bool containsNativeAbi() const { return containsNativeAbi_; }

    void setDependencyInterfaceHashes(
        std::unordered_map<string, std::uint64_t> dependencyInterfaceHashes);
    void setCompileProfile(string targetTriple, int optLevel, bool debugInfo);
    void setCompileProfile(std::string targetTriple, int optLevel, bool debugInfo) {
        setCompileProfile(string(std::move(targetTriple)), optLevel, debugInfo);
    }
    void setBitcode(ByteBuffer bitcode);
    void setObjectCode(ByteBuffer objectCode);
    void setContainsNativeAbi(bool containsNativeAbi);
};

}  // namespace lona
