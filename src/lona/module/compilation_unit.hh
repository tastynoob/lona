#pragma once

#include "generic_instance.hh"
#include "lona/ast/astnode.hh"
#include "lona/source/source_manager.hh"
#include "module_interface.hh"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
    enum class TopLevelLookupKind {
        NotFound,
        Module,
        Type,
        Trait,
        Function,
        Global,
    };

    struct ImportedModule {
        string path;
        string moduleKey;
        string moduleName;
        string modulePath;
        const ModuleInterface *interface = nullptr;
        const CompilationUnit *unit = nullptr;
    };

    struct TopLevelLookup {
        TopLevelLookupKind kind = TopLevelLookupKind::NotFound;
        const ImportedModule *importedModule = nullptr;
        const ModuleInterface::TypeDecl *typeDecl = nullptr;
        const ModuleInterface::TraitDecl *traitDecl = nullptr;
        const ModuleInterface::FunctionDecl *functionDecl = nullptr;
        const ModuleInterface::GlobalDecl *globalDecl = nullptr;
        string resolvedName;

        bool found() const { return kind != TopLevelLookupKind::NotFound; }
        bool isModule() const { return kind == TopLevelLookupKind::Module; }
        bool isType() const { return kind == TopLevelLookupKind::Type; }
        bool isTrait() const { return kind == TopLevelLookupKind::Trait; }
        bool isFunction() const { return kind == TopLevelLookupKind::Function; }
        bool isGlobal() const { return kind == TopLevelLookupKind::Global; }
    };

    struct VisibleTraitImpl {
        const ModuleInterface::TraitImplDecl *implDecl = nullptr;
        const ImportedModule *importedModule = nullptr;

        bool isImported() const { return importedModule != nullptr; }
    };

private:
    string path_;
    string moduleKey_;
    string moduleName_;
    string modulePath_;
    const SourceBuffer *source_ = nullptr;
    AstNode *syntaxTree_ = nullptr;
    CompilationUnitStage stage_ = CompilationUnitStage::Discovered;
    std::shared_ptr<ModuleInterface> moduleInterface_;
    std::unordered_map<string, ImportedModule> importedModules_;
    std::unordered_map<string, string> localTypeBindings_;
    std::unordered_map<string, string> localTraitBindings_;
    std::unordered_map<string, string> localFunctionBindings_;
    std::unordered_map<string, string> localGlobalBindings_;
    mutable std::unordered_map<const TypeNode *, TypeClass *> resolvedTypes_;
    mutable std::unordered_set<GenericInstanceKey, GenericInstanceKeyHash>
        materializingAppliedStructs_;
    mutable std::vector<GenericInstanceArtifactRecord>
        recordedGenericInstances_;
    mutable bool hashesReady_ = false;
    mutable std::uint64_t interfaceHash_ = 0;
    mutable std::uint64_t implementationHash_ = 0;

    void invalidateCaches();
    void ensureHashes() const;

public:
    explicit CompilationUnit(const SourceBuffer &source);

    const string &path() const { return path_; }
    const string &moduleKey() const { return moduleKey_; }
    const string &moduleName() const { return moduleName_; }
    const string &modulePath() const { return modulePath_; }
    string exportNamespacePrefix() const;
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
    bool interfaceCollected() const {
        return moduleInterface_ && moduleInterface_->collected();
    }
    AstNode *syntaxTree() const { return syntaxTree_; }
    AstNode *requireSyntaxTree() const;
    ModuleInterface *interface() { return moduleInterface_.get(); }
    const ModuleInterface *interface() const { return moduleInterface_.get(); }

    void attachInterface(std::shared_ptr<ModuleInterface> moduleInterface);
    void refreshSource(const SourceBuffer &source);
    void setModulePath(string modulePath);
    void setModulePath(std::string modulePath) {
        setModulePath(string(std::move(modulePath)));
    }
    void setSyntaxTree(AstNode *tree);
    void markDependenciesScanned();
    void markInterfaceCollected();
    void markCompiled();
    void clearImportedModules();
    void clearLocalBindings();
    bool addImportedModule(string alias, const CompilationUnit &unit);
    bool addImportedModule(std::string alias, const CompilationUnit &unit) {
        return addImportedModule(string(std::move(alias)), unit);
    }
    const ImportedModule *findImportedModule(const ::string &alias) const;
    const ImportedModule *findImportedModule(const std::string &alias) const {
        return findImportedModule(string(alias));
    }
    const std::unordered_map<string, ImportedModule> &importedModules() const {
        return importedModules_;
    }
    const ImportedModule *findImportedModuleByInterface(
        const ModuleInterface *interface) const;
    bool importsModule(const ::string &alias) const;
    bool importsModule(const std::string &alias) const {
        return importsModule(string(alias));
    }

    void clearInterface();
    bool bindLocalType(string localName, string resolvedName);
    bool bindLocalType(std::string localName, string resolvedName) {
        return bindLocalType(string(std::move(localName)),
                             std::move(resolvedName));
    }
    bool bindLocalTrait(string localName, string resolvedName);
    bool bindLocalTrait(std::string localName, string resolvedName) {
        return bindLocalTrait(string(std::move(localName)),
                              std::move(resolvedName));
    }
    bool bindLocalFunction(string localName, string resolvedName);
    bool bindLocalFunction(std::string localName, string resolvedName) {
        return bindLocalFunction(string(std::move(localName)),
                                 std::move(resolvedName));
    }
    bool bindLocalGlobal(string localName, string resolvedName);
    bool bindLocalGlobal(std::string localName, string resolvedName) {
        return bindLocalGlobal(string(std::move(localName)),
                               std::move(resolvedName));
    }
    const string *findLocalType(const ::string &localName) const;
    const string *findLocalType(const std::string &localName) const {
        return findLocalType(string(localName));
    }
    const string *findLocalTrait(const ::string &localName) const;
    const string *findLocalTrait(const std::string &localName) const {
        return findLocalTrait(string(localName));
    }
    const string *findLocalFunction(const ::string &localName) const;
    const string *findLocalFunction(const std::string &localName) const {
        return findLocalFunction(string(localName));
    }
    const string *findLocalGlobal(const ::string &localName) const;
    const string *findLocalGlobal(const std::string &localName) const {
        return findLocalGlobal(string(localName));
    }
    const AstVarDef *findTopLevelInline(const ::string &localName) const;
    const AstVarDef *findTopLevelInline(const std::string &localName) const {
        return findTopLevelInline(string(localName));
    }
    TopLevelLookup lookupTopLevelName(const ::string &name) const;
    TopLevelLookup lookupTopLevelName(const std::string &name) const {
        return lookupTopLevelName(string(name));
    }
    TopLevelLookup lookupTopLevelName(const ImportedModule &moduleNamespace,
                                      const ::string &name) const;
    TopLevelLookup lookupTopLevelName(const ImportedModule &moduleNamespace,
                                      const std::string &name) const {
        return lookupTopLevelName(moduleNamespace, string(name));
    }
    const ModuleInterface::TraitDecl *findVisibleTraitByResolvedName(
        const ::string &resolvedName) const;
    const ModuleInterface::TraitDecl *findVisibleTraitByResolvedName(
        const std::string &resolvedName) const {
        return findVisibleTraitByResolvedName(string(resolvedName));
    }
    std::vector<VisibleTraitImpl> findVisibleTraitImpls(
        const ::string &traitName, const ::string &selfTypeSpelling) const;
    std::vector<VisibleTraitImpl> findVisibleTraitImpls(
        const std::string &traitName, const std::string &selfTypeSpelling) const {
        return findVisibleTraitImpls(string(traitName), string(selfTypeSpelling));
    }
    std::vector<VisibleTraitImpl> findVisibleTraitImpls(
        const ::string &traitName, TypeClass *selfType) const;
    std::vector<VisibleTraitImpl> findVisibleTraitImpls(
        const std::string &traitName, TypeClass *selfType) const {
        return findVisibleTraitImpls(string(traitName), selfType);
    }
    std::vector<VisibleTraitImpl> findVisibleTraitImpls(
        TypeClass *selfType) const;
    TypeClass *findResolvedType(TypeNode *node) const;
    void cacheResolvedType(TypeNode *node, TypeClass *type) const;
    void clearResolvedTypes();
    void recordGenericInstance(GenericInstanceArtifactRecord record) const;
    const std::vector<GenericInstanceArtifactRecord> &recordedGenericInstances()
        const {
        return recordedGenericInstances_;
    }
    TypeClass *resolveType(TypeTable *typeTable, TypeNode *node) const;
    bool ownsTypeDecl(const ModuleInterface::TypeDecl *typeDecl) const;
    const CompilationUnit *ownerUnitForTypeDecl(
        const ModuleInterface::TypeDecl *typeDecl) const;
    const CompilationUnit *contextUnitForInterface(
        const ModuleInterface *ownerInterface) const;
    std::uint64_t visibleImportInterfaceHash() const;
    std::uint64_t visibleTraitImplHash() const;
    StructType *materializeAppliedStructType(
        TypeTable *typeTable, const ModuleInterface::TypeDecl &typeDecl,
        std::vector<TypeClass *> appliedTypeArgs,
        const CompilationUnit &contextUnit) const;
    StructType *materializeLocalAppliedStructType(
        TypeTable *typeTable, const ModuleInterface::TypeDecl &typeDecl,
        std::vector<TypeClass *> appliedTypeArgs) const;
};

}  // namespace lona
