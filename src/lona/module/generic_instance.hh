#pragma once

#include "lona/util/string.hh"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lona {

enum class GenericInstanceKind {
    Function,
    Struct,
    Method,
};

struct GenericTemplateRevision {
    std::uint64_t ownerInterfaceHash = 0;
    std::uint64_t ownerImplementationHash = 0;
    std::uint64_t ownerVisibleImportHash = 0;
    std::uint64_t boundVisibleStateHash = 0;

    bool operator==(const GenericTemplateRevision &other) const {
        return ownerInterfaceHash == other.ownerInterfaceHash &&
               ownerImplementationHash == other.ownerImplementationHash &&
               ownerVisibleImportHash == other.ownerVisibleImportHash &&
               boundVisibleStateHash == other.boundVisibleStateHash;
    }
};

struct GenericInstanceKey {
    string requesterModuleKey;
    string ownerModuleKey;
    GenericInstanceKind kind = GenericInstanceKind::Function;
    string templateName;
    string methodName;
    std::vector<string> concreteTypeArgs;

    bool operator==(const GenericInstanceKey &other) const {
        return ownerModuleKey == other.ownerModuleKey &&
               kind == other.kind && templateName == other.templateName &&
               methodName == other.methodName &&
               concreteTypeArgs == other.concreteTypeArgs;
    }
};

struct GenericInstanceArtifactRecord {
    GenericInstanceKey key;
    GenericTemplateRevision revision;
    std::vector<string> emittedSymbolNames;
};

inline std::size_t
combineGenericInstanceHash(std::size_t seed, std::size_t value) {
    constexpr std::size_t kSeed = 0x9e3779b97f4a7c15ULL;
    seed ^= value + kSeed + (seed << 6) + (seed >> 2);
    return seed;
}

struct GenericInstanceKeyHash {
    std::size_t operator()(const GenericInstanceKey &key) const {
        std::size_t seed = 0;
        seed = combineGenericInstanceHash(seed,
                                          std::hash<string>{}(key.ownerModuleKey));
        seed = combineGenericInstanceHash(
            seed, static_cast<std::size_t>(key.kind));
        seed = combineGenericInstanceHash(seed,
                                          std::hash<string>{}(key.templateName));
        seed = combineGenericInstanceHash(seed,
                                          std::hash<string>{}(key.methodName));
        for (const auto &arg : key.concreteTypeArgs) {
            seed = combineGenericInstanceHash(seed, std::hash<string>{}(arg));
        }
        return seed;
    }
};

struct GenericInstanceEmissionOwner {
    string moduleKey;
    std::vector<string> symbolNames;
};

class GenericInstanceRegistry {
    std::unordered_map<GenericInstanceKey, GenericInstanceEmissionOwner,
                       GenericInstanceKeyHash>
        owners_;

public:
    const GenericInstanceEmissionOwner *find(
        const GenericInstanceKey &key) const {
        auto found = owners_.find(key);
        if (found == owners_.end()) {
            return nullptr;
        }
        return &found->second;
    }

    const string *emitterModuleKey(const GenericInstanceKey &key) const {
        auto *owner = find(key);
        if (!owner || owner->moduleKey.empty()) {
            return nullptr;
        }
        return &owner->moduleKey;
    }

    bool shouldEmitIn(const GenericInstanceKey &key,
                      const string &moduleKey) const {
        auto *emitter = emitterModuleKey(key);
        return emitter == nullptr || *emitter == moduleKey;
    }

    void claim(const GenericInstanceKey &key, string moduleKey) {
        auto [it, inserted] =
            owners_.emplace(key,
                            GenericInstanceEmissionOwner{moduleKey, {}});
        if (!inserted && it->second.moduleKey.empty()) {
            it->second.moduleKey = std::move(moduleKey);
        }
    }

    void noteEmission(const GenericInstanceKey &key, string moduleKey,
                      const std::vector<string> &symbolNames) {
        auto [it, _] =
            owners_.emplace(key,
                            GenericInstanceEmissionOwner{moduleKey, {}});
        if (it->second.moduleKey.empty()) {
            it->second.moduleKey = std::move(moduleKey);
        }
        for (const auto &symbol : symbolNames) {
            auto found =
                std::find(it->second.symbolNames.begin(),
                          it->second.symbolNames.end(), symbol);
            if (found == it->second.symbolNames.end()) {
                it->second.symbolNames.push_back(symbol);
            }
        }
    }
};

}  // namespace lona
