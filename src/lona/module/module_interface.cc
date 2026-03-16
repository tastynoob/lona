#include "module_interface.hh"
#include <functional>
#include <utility>

namespace lona {

ModuleInterface::ModuleInterface(std::string sourcePath, std::string moduleKey,
                                 std::string moduleName, std::uint64_t sourceHash)
    : sourcePath_(std::move(sourcePath)),
      moduleKey_(std::move(moduleKey)),
      moduleName_(std::move(moduleName)),
      sourceHash_(sourceHash) {}

void
ModuleInterface::refresh(std::string sourcePath, std::string moduleKey,
                         std::string moduleName, std::uint64_t sourceHash) {
    const bool changed = sourcePath_ != sourcePath || moduleKey_ != moduleKey ||
        moduleName_ != moduleName || sourceHash_ != sourceHash;
    sourcePath_ = std::move(sourcePath);
    moduleKey_ = std::move(moduleKey);
    moduleName_ = std::move(moduleName);
    sourceHash_ = sourceHash;
    if (changed) {
        clear();
    }
}

void
ModuleInterface::clear() {
    collected_ = false;
    namespaced_ = false;
    localTypeNames_.clear();
    localFunctionNames_.clear();
}

bool
ModuleInterface::bindLocalType(std::string localName, std::string resolvedName) {
    return localTypeNames_
        .emplace(std::move(localName), std::move(resolvedName))
        .second;
}

bool
ModuleInterface::bindLocalFunction(std::string localName, std::string resolvedName) {
    return localFunctionNames_
        .emplace(std::move(localName), std::move(resolvedName))
        .second;
}

const std::string *
ModuleInterface::findLocalType(const std::string &localName) const {
    auto found = localTypeNames_.find(localName);
    if (found == localTypeNames_.end()) {
        return nullptr;
    }
    return &found->second;
}

const std::string *
ModuleInterface::findLocalFunction(const std::string &localName) const {
    auto found = localFunctionNames_.find(localName);
    if (found == localFunctionNames_.end()) {
        return nullptr;
    }
    return &found->second;
}

std::uint64_t
hashModuleSource(const std::string &content) {
    return std::hash<std::string>{}(content);
}

}  // namespace lona
