#pragma once

#include "module_interface.hh"
#include "lona/source/source_manager.hh"
#include <memory>
#include <string>
#include <unordered_map>

namespace lona {

class ModuleCache {
    std::unordered_map<std::string, std::shared_ptr<ModuleInterface>> interfaces_;

public:
    std::shared_ptr<ModuleInterface> getOrCreate(const SourceBuffer &source,
                                                 const std::string &moduleKey,
                                                 const std::string &moduleName);
    const ModuleInterface *find(const std::string &path) const;
    void clear();
};

}  // namespace lona
