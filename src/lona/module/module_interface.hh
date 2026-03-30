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
class TupleType;
class AstNode;

class ModuleInterface {
public:
    enum class TopLevelLookupKind {
        NotFound,
        Type,
        Function,
        Global,
    };

    struct TypeDecl {
        string localName;
        string exportedName;
        StructDeclKind declKind = StructDeclKind::Native;
        TypeClass *type = nullptr;
    };

    struct FunctionDecl {
        string localName;
        string symbolName;
        AbiKind abiKind = AbiKind::Native;
        FuncType *type = nullptr;
        std::vector<string> paramNames;
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
        const FunctionDecl *functionDecl = nullptr;
        const GlobalDecl *globalDecl = nullptr;

        bool found() const { return kind != TopLevelLookupKind::NotFound; }
        bool isType() const { return kind == TopLevelLookupKind::Type; }
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
    std::unordered_map<string, FunctionDecl> localFunctions_;
    std::unordered_map<string, GlobalDecl> localGlobals_;

    string exportedNameFor(const ::string &localName) const;
    string functionSymbolNameFor(const ::string &localName,
                                 AbiKind abiKind) const;
    string globalSymbolNameFor(const ::string &localName, bool isExtern) const;

public:
    ModuleInterface(string sourcePath, string moduleKey,
                    string moduleName, std::uint64_t sourceHash);
    ModuleInterface(std::string sourcePath, std::string moduleKey,
                    std::string moduleName, std::uint64_t sourceHash)
        : ModuleInterface(string(std::move(sourcePath)),
                          string(std::move(moduleKey)),
                          string(std::move(moduleName)),
                          sourceHash) {}
    ~ModuleInterface();

    const string &sourcePath() const { return sourcePath_; }
    const string &moduleKey() const { return moduleKey_; }
    const string &moduleName() const { return moduleName_; }
    std::uint64_t sourceHash() const { return sourceHash_; }

    void refresh(string sourcePath, string moduleKey,
                 string moduleName, std::uint64_t sourceHash);
    void refresh(std::string sourcePath, std::string moduleKey,
                 std::string moduleName, std::uint64_t sourceHash) {
        refresh(string(std::move(sourcePath)),
                string(std::move(moduleKey)),
                string(std::move(moduleName)),
                sourceHash);
    }

    bool collected() const { return collected_; }
    void markCollected() { collected_ = true; }

    void clear();
    StructType *declareStructType(const ::string &localName,
                                  StructDeclKind declKind = StructDeclKind::Native);
    StructType *declareStructType(const std::string &localName,
                                  StructDeclKind declKind = StructDeclKind::Native) {
        return declareStructType(string(localName), declKind);
    }
    bool declareFunction(string localName, FuncType *type,
                         std::vector<string> paramNames = {});
    bool declareFunction(std::string localName, FuncType *type,
                         std::vector<string> paramNames = {}) {
        return declareFunction(string(std::move(localName)), type,
                               std::move(paramNames));
    }
    bool declareGlobal(string localName, TypeClass *type, bool isExtern = false);
    bool declareGlobal(std::string localName, TypeClass *type, bool isExtern = false) {
        return declareGlobal(string(std::move(localName)), type, isExtern);
    }
    PointerType *getOrCreatePointerType(TypeClass *pointeeType);
    IndexablePointerType *getOrCreateIndexablePointerType(TypeClass *elementType);
    ConstType *getOrCreateConstType(TypeClass *baseType);
    ArrayType *getOrCreateArrayType(TypeClass *elementType,
                                    std::vector<AstNode *> dimensions = {});
    TupleType *getOrCreateTupleType(const std::vector<TypeClass *> &itemTypes);
    FuncType *getOrCreateFunctionType(const std::vector<TypeClass *> &argTypes,
                                      TypeClass *retType,
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
    const GlobalDecl *findGlobal(const ::string &localName) const;
    const GlobalDecl *findGlobal(const std::string &localName) const {
        return findGlobal(string(localName));
    }
    TopLevelLookup lookupTopLevelName(const ::string &localName) const;
    TopLevelLookup lookupTopLevelName(const std::string &localName) const {
        return lookupTopLevelName(string(localName));
    }
    const std::unordered_map<string, TypeDecl> &types() const { return localTypes_; }
    const std::unordered_map<string, FunctionDecl> &functions() const {
        return localFunctions_;
    }
    const std::unordered_map<string, GlobalDecl> &globals() const {
        return localGlobals_;
    }
};

std::uint64_t hashModuleSource(const std::string &content);

}  // namespace lona
