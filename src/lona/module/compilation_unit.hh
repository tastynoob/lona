#pragma once

#include "lona/ast/astnode.hh"
#include "module_interface.hh"
#include "lona/source/source_manager.hh"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace lona {

class TypeClass;
class TypeTable;

enum class CompilationUnitStage {
    Discovered,
    Parsed,
    DependenciesScanned,
    InterfaceCollected,
    Compiled,
};

class CompilationUnit {
public:
    struct ImportedModule {
        std::string path;
        std::string moduleKey;
        std::string moduleName;
        const ModuleInterface *interface = nullptr;
    };

private:
    std::string path_;
    std::string moduleKey_;
    std::string moduleName_;
    const SourceBuffer *source_ = nullptr;
    AstNode *syntaxTree_ = nullptr;
    CompilationUnitStage stage_ = CompilationUnitStage::Discovered;
    std::shared_ptr<ModuleInterface> moduleInterface_;
    std::unordered_map<std::string, ImportedModule> importedModules_;
    mutable std::unordered_map<const TypeNode *, TypeClass *> resolvedTypes_;
    mutable bool hashesReady_ = false;
    mutable std::uint64_t interfaceHash_ = 0;
    mutable std::uint64_t implementationHash_ = 0;

    void invalidateCaches();
    void ensureHashes() const;

public:
    explicit CompilationUnit(const SourceBuffer &source);

    const std::string &path() const { return path_; }
    const std::string &moduleKey() const { return moduleKey_; }
    const std::string &moduleName() const { return moduleName_; }
    const SourceBuffer &source() const;
    std::uint64_t sourceHash() const;
    std::uint64_t interfaceHash() const;
    std::uint64_t implementationHash() const;
    CompilationUnitStage stage() const { return stage_; }
    bool hasSyntaxTree() const { return syntaxTree_ != nullptr; }
    bool dependenciesScanned() const {
        return static_cast<int>(stage_) >=
            static_cast<int>(CompilationUnitStage::DependenciesScanned);
    }
    bool interfaceCollected(bool namespaced) const {
        return moduleInterface_ && moduleInterface_->collected(namespaced);
    }
    AstNode *syntaxTree() const { return syntaxTree_; }
    ModuleInterface *interface() { return moduleInterface_.get(); }
    const ModuleInterface *interface() const { return moduleInterface_.get(); }

    void attachInterface(std::shared_ptr<ModuleInterface> moduleInterface);
    void refreshSource(const SourceBuffer &source);
    void setSyntaxTree(AstNode *tree);
    void markDependenciesScanned();
    void markInterfaceCollected(bool namespaced);
    void markCompiled();
    void clearImportedModules();
    bool addImportedModule(std::string alias, const CompilationUnit &unit);
    const ImportedModule *findImportedModule(const std::string &alias) const;
    bool importsModule(const std::string &alias) const;

    void clearInterface();
    bool bindLocalType(std::string localName, std::string resolvedName);
    bool bindLocalFunction(std::string localName, std::string resolvedName);
    const std::string *findLocalType(const std::string &localName) const;
    const std::string *findLocalFunction(const std::string &localName) const;
    TypeClass *findResolvedType(TypeNode *node) const;
    void cacheResolvedType(TypeNode *node, TypeClass *type) const;
    TypeClass *resolveType(TypeTable *typeTable, TypeNode *node) const;
};

}  // namespace lona
