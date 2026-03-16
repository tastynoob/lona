#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace lona {

class ModuleInterface {
    std::string sourcePath_;
    std::string moduleKey_;
    std::string moduleName_;
    std::uint64_t sourceHash_ = 0;
    bool collected_ = false;
    bool namespaced_ = false;
    std::unordered_map<std::string, std::string> localTypeNames_;
    std::unordered_map<std::string, std::string> localFunctionNames_;

public:
    ModuleInterface(std::string sourcePath, std::string moduleKey,
                    std::string moduleName, std::uint64_t sourceHash);

    const std::string &sourcePath() const { return sourcePath_; }
    const std::string &moduleKey() const { return moduleKey_; }
    const std::string &moduleName() const { return moduleName_; }
    std::uint64_t sourceHash() const { return sourceHash_; }

    void refresh(std::string sourcePath, std::string moduleKey,
                 std::string moduleName, std::uint64_t sourceHash);

    bool collected(bool namespaced) const {
        return collected_ && namespaced_ == namespaced;
    }
    void markCollected(bool namespaced) {
        collected_ = true;
        namespaced_ = namespaced;
    }

    void clear();
    bool bindLocalType(std::string localName, std::string resolvedName);
    bool bindLocalFunction(std::string localName, std::string resolvedName);
    const std::string *findLocalType(const std::string &localName) const;
    const std::string *findLocalFunction(const std::string &localName) const;
};

std::uint64_t hashModuleSource(const std::string &content);

}  // namespace lona
