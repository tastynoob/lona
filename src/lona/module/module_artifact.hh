#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lona {

class ModuleArtifact {
public:
    using ByteBuffer = std::vector<std::uint8_t>;

private:
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
    ByteBuffer bitcode_;
    ByteBuffer objectCode_;
    bool containsNativeAbi_ = false;

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
    const ByteBuffer &bitcode() const { return bitcode_; }
    bool hasBitcode() const { return !bitcode_.empty(); }
    const ByteBuffer &objectCode() const { return objectCode_; }
    bool hasObjectCode() const { return !objectCode_.empty(); }
    bool containsNativeAbi() const { return containsNativeAbi_; }

    void setDependencyInterfaceHashes(
        std::unordered_map<std::string, std::uint64_t> dependencyInterfaceHashes);
    void setCompileProfile(std::string targetTriple, int optLevel, bool debugInfo);
    void setBitcode(ByteBuffer bitcode);
    void setObjectCode(ByteBuffer objectCode);
    void setContainsNativeAbi(bool containsNativeAbi);
};

}  // namespace lona
