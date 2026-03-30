#pragma once

#include "lona/source/source_manager.hh"
#include "module_interface.hh"
#include <memory>
#include <string>
#include <unordered_map>

namespace lona {

class ModuleCache {
    std::unordered_map<string, std::shared_ptr<ModuleInterface>> interfaces_;

public:
    std::shared_ptr<ModuleInterface> getOrCreate(const SourceBuffer &source,
                                                 const string &moduleKey,
                                                 const string &moduleName);
    std::shared_ptr<ModuleInterface> getOrCreate(
        const SourceBuffer &source, const std::string &moduleKey,
        const std::string &moduleName) {
        return getOrCreate(source, string(moduleKey), string(moduleName));
    }
    const ModuleInterface *find(const string &path) const;
    const ModuleInterface *find(const std::string &path) const {
        return find(string(path));
    }
    void clear();
};

}  // namespace lona
