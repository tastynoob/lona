#pragma once

#include "lona/ast/astnode.hh"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lona {

class TypeClass;
class StructType;
class FuncType;
class ArrayType;
class PointerType;
class IndexablePointerType;
class ConstType;
class DynTraitType;
class TupleType;
class AnyType;
class AstNode;

class ModuleInterface {
public:
    enum class TopLevelLookupKind {
        NotFound,
        Type,
        Trait,
        Function,
        Global,
    };

    struct GenericParamDecl {
        string localName;
        string boundTraitName;
    };

    struct ImportedModuleDecl {
        string localName;
        string moduleKey;
        string moduleName;
        const ModuleInterface *interface = nullptr;
    };

    struct MethodTemplateDecl {
        string localName;
        AccessKind receiverAccess = AccessKind::GetOnly;
        std::vector<string> paramNames;
        std::vector<BindingKind> paramBindingKinds;
        std::vector<TypeNode *> paramTypeNodes;
        std::vector<string> paramTypeSpellings;
        TypeNode *returnTypeNode = nullptr;
        string returnTypeSpelling = "void";
        std::vector<GenericParamDecl> typeParams;

        bool isGeneric() const { return !typeParams.empty(); }
    };

    struct TypeDecl {
        string localName;
        string exportedName;
        StructDeclKind declKind = StructDeclKind::Native;
        TypeClass *type = nullptr;
        std::vector<GenericParamDecl> typeParams;
        std::vector<MethodTemplateDecl> methodTemplates;

        bool isGeneric() const { return !typeParams.empty(); }
    };

    struct TraitMethodDecl {
        string localName;
        AccessKind receiverAccess = AccessKind::GetOnly;
        std::vector<string> paramNames;
        std::vector<BindingKind> paramBindingKinds;
        std::vector<string> paramTypeSpellings;
        string returnTypeSpelling;
    };

    struct TraitDecl {
        string localName;
        string exportedName;
        std::vector<TraitMethodDecl> methods;

        const TraitMethodDecl *findMethod(const ::string &name) const {
            for (const auto &method : methods) {
                if (method.localName == name) {
                    return &method;
                }
            }
            return nullptr;
        }
    };

    struct TraitImplDecl {
        string selfTypeSpelling;
        string traitName;
        bool hasBody = false;
        std::vector<GenericParamDecl> typeParams;

        bool isGeneric() const { return !typeParams.empty(); }
    };

    struct FunctionDecl {
        string localName;
        string symbolName;
        AbiKind abiKind = AbiKind::Native;
        FuncType *type = nullptr;
        std::vector<string> paramNames;
        std::vector<BindingKind> paramBindingKinds;
        std::vector<TypeNode *> paramTypeNodes;
        std::vector<string> paramTypeSpellings;
        TypeNode *returnTypeNode = nullptr;
        string returnTypeSpelling;
        std::vector<GenericParamDecl> typeParams;

        bool isGeneric() const { return !typeParams.empty(); }
    };

    struct GlobalDecl {
        string localName;
        string symbolName;
        bool isExtern = false;
        TypeClass *type = nullptr;
    };

    struct TopLevelLookup {
        TopLevelLookupKind kind = TopLevelLookupKind::NotFound;
        const TypeDecl *typeDecl = nullptr;
        const TraitDecl *traitDecl = nullptr;
        const FunctionDecl *functionDecl = nullptr;
        const GlobalDecl *globalDecl = nullptr;

        bool found() const { return kind != TopLevelLookupKind::NotFound; }
        bool isType() const { return kind == TopLevelLookupKind::Type; }
        bool isTrait() const { return kind == TopLevelLookupKind::Trait; }
        bool isFunction() const { return kind == TopLevelLookupKind::Function; }
        bool isGlobal() const { return kind == TopLevelLookupKind::Global; }
    };

private:
    string sourcePath_;
    string moduleKey_;
    string moduleName_;
    std::uint64_t sourceHash_ = 0;
    bool collected_ = false;
    std::vector<std::unique_ptr<TypeClass>> ownedTypes_;
    std::unordered_map<string, TypeClass *> derivedTypes_;
    std::unordered_map<string, TypeDecl> localTypes_;
    std::unordered_map<string, TraitDecl> localTraits_;
    std::vector<TraitImplDecl> traitImpls_;
    std::unordered_map<string, FunctionDecl> localFunctions_;
    std::unordered_map<string, GlobalDecl> localGlobals_;
    std::unordered_map<string, ImportedModuleDecl> importedModules_;

    string exportedNameFor(const ::string &localName) const;
    string functionSymbolNameFor(const ::string &localName,
                                 AbiKind abiKind) const;
    string globalSymbolNameFor(const ::string &localName, bool isExtern) const;

public:
    ModuleInterface(string sourcePath, string moduleKey, string moduleName,
                    std::uint64_t sourceHash);
    ModuleInterface(std::string sourcePath, std::string moduleKey,
                    std::string moduleName, std::uint64_t sourceHash)
        : ModuleInterface(string(std::move(sourcePath)),
                          string(std::move(moduleKey)),
                          string(std::move(moduleName)), sourceHash) {}
    ~ModuleInterface();

    const string &sourcePath() const { return sourcePath_; }
    const string &moduleKey() const { return moduleKey_; }
    const string &moduleName() const { return moduleName_; }
    std::uint64_t sourceHash() const { return sourceHash_; }

    void refresh(string sourcePath, string moduleKey, string moduleName,
                 std::uint64_t sourceHash);
    void refresh(std::string sourcePath, std::string moduleKey,
                 std::string moduleName, std::uint64_t sourceHash) {
        refresh(string(std::move(sourcePath)), string(std::move(moduleKey)),
                string(std::move(moduleName)), sourceHash);
    }

    bool collected() const { return collected_; }
    void markCollected() { collected_ = true; }

    void clear();
    StructType *declareStructType(
        const ::string &localName,
        StructDeclKind declKind = StructDeclKind::Native,
        std::vector<GenericParamDecl> typeParams = {});
    StructType *declareStructType(
        const std::string &localName,
        StructDeclKind declKind = StructDeclKind::Native,
        std::vector<GenericParamDecl> typeParams = {}) {
        return declareStructType(string(localName), declKind,
                                 std::move(typeParams));
    }
    bool declareFunction(string localName, FuncType *type,
                         std::vector<string> paramNames = {},
                         std::vector<BindingKind> paramBindingKinds = {},
                         std::vector<TypeNode *> paramTypeNodes = {},
                         std::vector<string> paramTypeSpellings = {},
                         TypeNode *returnTypeNode = nullptr,
                         string returnTypeSpelling = "void",
                         std::vector<GenericParamDecl> typeParams = {});
    bool declareFunction(std::string localName, FuncType *type,
                         std::vector<string> paramNames = {},
                         std::vector<BindingKind> paramBindingKinds = {},
                         std::vector<TypeNode *> paramTypeNodes = {},
                         std::vector<string> paramTypeSpellings = {},
                         TypeNode *returnTypeNode = nullptr,
                         string returnTypeSpelling = "void",
                         std::vector<GenericParamDecl> typeParams = {}) {
        return declareFunction(string(std::move(localName)), type,
                               std::move(paramNames),
                               std::move(paramBindingKinds),
                               std::move(paramTypeNodes),
                               std::move(paramTypeSpellings),
                               returnTypeNode,
                               std::move(returnTypeSpelling),
                               std::move(typeParams));
    }
    bool declareGlobal(string localName, TypeClass *type,
                       bool isExtern = false);
    bool declareGlobal(std::string localName, TypeClass *type,
                       bool isExtern = false) {
        return declareGlobal(string(std::move(localName)), type, isExtern);
    }
    bool declareTrait(string localName,
                      std::vector<TraitMethodDecl> methods = {});
    bool declareTrait(std::string localName,
                      std::vector<TraitMethodDecl> methods = {}) {
        return declareTrait(string(std::move(localName)), std::move(methods));
    }
    bool defineTraitMethods(string localName,
                            std::vector<TraitMethodDecl> methods);
    bool defineTraitMethods(std::string localName,
                            std::vector<TraitMethodDecl> methods) {
        return defineTraitMethods(string(std::move(localName)),
                                  std::move(methods));
    }
    bool declareTraitImpl(string selfTypeSpelling, string traitName,
                          bool hasBody = false,
                          std::vector<GenericParamDecl> typeParams = {});
    bool declareTraitImpl(std::string selfTypeSpelling, std::string traitName,
                          bool hasBody = false,
                          std::vector<GenericParamDecl> typeParams = {}) {
        return declareTraitImpl(string(std::move(selfTypeSpelling)),
                                string(std::move(traitName)), hasBody,
                                std::move(typeParams));
    }
    bool declareStructMethodTemplate(string structLocalName,
                                     MethodTemplateDecl method);
    bool declareStructMethodTemplate(std::string structLocalName,
                                     MethodTemplateDecl method) {
        return declareStructMethodTemplate(string(std::move(structLocalName)),
                                           std::move(method));
    }
    bool declareImportedModule(string localName, string moduleKey,
                               string moduleName,
                               const ModuleInterface *interface = nullptr);
    bool declareImportedModule(std::string localName, std::string moduleKey,
                               std::string moduleName,
                               const ModuleInterface *interface = nullptr) {
        return declareImportedModule(string(std::move(localName)),
                                     string(std::move(moduleKey)),
                                     string(std::move(moduleName)), interface);
    }
    AnyType *getOrCreateAnyType();
    StructType *getOrCreateAppliedStructType(
        const ::string &appliedName, StructDeclKind declKind,
        string appliedTemplateName = {},
        std::vector<TypeClass *> appliedTypeArgs = {});
    StructType *getOrCreateAppliedStructType(
        const std::string &appliedName, StructDeclKind declKind,
        string appliedTemplateName = {},
        std::vector<TypeClass *> appliedTypeArgs = {}) {
        return getOrCreateAppliedStructType(
            string(appliedName), declKind, std::move(appliedTemplateName),
            std::move(appliedTypeArgs));
    }
    PointerType *getOrCreatePointerType(TypeClass *pointeeType);
    IndexablePointerType *getOrCreateIndexablePointerType(
        TypeClass *elementType);
    DynTraitType *getOrCreateDynTraitType(const ::string &traitName);
    DynTraitType *getOrCreateDynTraitType(const std::string &traitName) {
        return getOrCreateDynTraitType(string(traitName));
    }
    ConstType *getOrCreateConstType(TypeClass *baseType);
    ArrayType *getOrCreateArrayType(TypeClass *elementType,
                                    std::vector<AstNode *> dimensions = {});
    TupleType *getOrCreateTupleType(const std::vector<TypeClass *> &itemTypes);
    FuncType *getOrCreateFunctionType(
        const std::vector<TypeClass *> &argTypes, TypeClass *retType,
        std::vector<BindingKind> argBindingKinds = {},
        AbiKind abiKind = AbiKind::Native);
    const TypeDecl *findType(const ::string &localName) const;
    const TypeDecl *findType(const std::string &localName) const {
        return findType(string(localName));
    }
    const FunctionDecl *findFunction(const ::string &localName) const;
    const FunctionDecl *findFunction(const std::string &localName) const {
        return findFunction(string(localName));
    }
    const TraitDecl *findTrait(const ::string &localName) const;
    const TraitDecl *findTrait(const std::string &localName) const {
        return findTrait(string(localName));
    }
    const TraitDecl *findTraitByExportedName(const ::string &exportedName) const;
    const TraitDecl *findTraitByExportedName(const std::string &exportedName) const {
        return findTraitByExportedName(string(exportedName));
    }
    const TraitImplDecl *findTraitImpl(const ::string &traitName,
                                       const ::string &selfTypeSpelling) const;
    const TraitImplDecl *findTraitImpl(const std::string &traitName,
                                       const std::string &selfTypeSpelling) const {
        return findTraitImpl(string(traitName), string(selfTypeSpelling));
    }
    const GlobalDecl *findGlobal(const ::string &localName) const;
    const GlobalDecl *findGlobal(const std::string &localName) const {
        return findGlobal(string(localName));
    }
    const ImportedModuleDecl *findImportedModule(const ::string &localName) const;
    const ImportedModuleDecl *findImportedModule(
        const std::string &localName) const {
        return findImportedModule(string(localName));
    }
    TypeClass *findDerivedType(const ::string &spelling) const;
    TypeClass *findDerivedType(const std::string &spelling) const {
        return findDerivedType(string(spelling));
    }
    TopLevelLookup lookupTopLevelName(const ::string &localName) const;
    TopLevelLookup lookupTopLevelName(const std::string &localName) const {
        return lookupTopLevelName(string(localName));
    }
    const std::unordered_map<string, TypeDecl> &types() const {
        return localTypes_;
    }
    const std::unordered_map<string, TraitDecl> &traits() const {
        return localTraits_;
    }
    const std::vector<TraitImplDecl> &traitImpls() const {
        return traitImpls_;
    }
    const std::unordered_map<string, FunctionDecl> &functions() const {
        return localFunctions_;
    }
    const std::unordered_map<string, GlobalDecl> &globals() const {
        return localGlobals_;
    }
    const std::unordered_map<string, ImportedModuleDecl> &importedModules() const {
        return importedModules_;
    }
};

std::uint64_t
hashModuleSource(const std::string &content);

}  // namespace lona
