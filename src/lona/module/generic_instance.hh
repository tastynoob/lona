#pragma once

#include "lona/util/string.hh"
#include <cstddef>
#include <cstdint>
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
        return requesterModuleKey == other.requesterModuleKey &&
               ownerModuleKey == other.ownerModuleKey &&
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
        seed = combineGenericInstanceHash(
            seed, std::hash<string>{}(key.requesterModuleKey));
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

}  // namespace lona
