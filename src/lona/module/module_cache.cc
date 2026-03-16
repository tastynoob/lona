#include "module_cache.hh"
#include <utility>

namespace lona {

std::shared_ptr<ModuleInterface>
ModuleCache::getOrCreate(const SourceBuffer &source, const std::string &moduleKey,
                         const std::string &moduleName) {
    auto inserted = interfaces_.emplace(
        source.path(),
        std::make_shared<ModuleInterface>(source.path(), moduleKey, moduleName,
                                          hashModuleSource(source.content())));
    if (!inserted.second) {
        inserted.first->second->refresh(source.path(), moduleKey, moduleName,
                                        hashModuleSource(source.content()));
    }
    return inserted.first->second;
}

const ModuleInterface *
ModuleCache::find(const std::string &path) const {
    auto found = interfaces_.find(path);
    if (found == interfaces_.end()) {
        return nullptr;
    }
    return found->second.get();
}

void
ModuleCache::clear() {
    interfaces_.clear();
}

}  // namespace lona
