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
class TupleType;
class AstNode;

class ModuleInterface {
public:
    enum class TopLevelLookupKind {
        NotFound,
        Type,
        Function,
    };

    struct TypeDecl {
        std::string localName;
        std::string exportedName;
        StructDeclKind declKind = StructDeclKind::Native;
        TypeClass *type = nullptr;
    };

    struct FunctionDecl {
        std::string localName;
        std::string symbolName;
        AbiKind abiKind = AbiKind::Native;
        FuncType *type = nullptr;
        std::vector<std::string> paramNames;
    };

    struct TopLevelLookup {
        TopLevelLookupKind kind = TopLevelLookupKind::NotFound;
        const TypeDecl *typeDecl = nullptr;
        const FunctionDecl *functionDecl = nullptr;

        bool found() const { return kind != TopLevelLookupKind::NotFound; }
        bool isType() const { return kind == TopLevelLookupKind::Type; }
        bool isFunction() const { return kind == TopLevelLookupKind::Function; }
    };

private:
    std::string sourcePath_;
    std::string moduleKey_;
    std::string moduleName_;
    std::uint64_t sourceHash_ = 0;
    bool collected_ = false;
    std::vector<std::unique_ptr<TypeClass>> ownedTypes_;
    std::unordered_map<std::string, TypeClass *> derivedTypes_;
    std::unordered_map<std::string, TypeDecl> localTypes_;
    std::unordered_map<std::string, FunctionDecl> localFunctions_;

    std::string exportedNameFor(const std::string &localName) const;
    std::string functionSymbolNameFor(const std::string &localName,
                                      AbiKind abiKind) const;

public:
    ModuleInterface(std::string sourcePath, std::string moduleKey,
                    std::string moduleName, std::uint64_t sourceHash);
    ~ModuleInterface();

    const std::string &sourcePath() const { return sourcePath_; }
    const std::string &moduleKey() const { return moduleKey_; }
    const std::string &moduleName() const { return moduleName_; }
    std::uint64_t sourceHash() const { return sourceHash_; }

    void refresh(std::string sourcePath, std::string moduleKey,
                 std::string moduleName, std::uint64_t sourceHash);

    bool collected() const { return collected_; }
    void markCollected() { collected_ = true; }

    void clear();
    StructType *declareStructType(const std::string &localName,
                                  StructDeclKind declKind = StructDeclKind::Native);
    bool declareFunction(std::string localName, FuncType *type,
                         std::vector<std::string> paramNames = {});
    PointerType *getOrCreatePointerType(TypeClass *pointeeType);
    IndexablePointerType *getOrCreateIndexablePointerType(TypeClass *elementType);
    ArrayType *getOrCreateArrayType(TypeClass *elementType,
                                    std::vector<AstNode *> dimensions = {});
    TupleType *getOrCreateTupleType(const std::vector<TypeClass *> &itemTypes);
    FuncType *getOrCreateFunctionType(const std::vector<TypeClass *> &argTypes,
                                      TypeClass *retType,
                                      std::vector<BindingKind> argBindingKinds = {},
                                      AbiKind abiKind = AbiKind::Native);
    const TypeDecl *findType(const std::string &localName) const;
    const FunctionDecl *findFunction(const std::string &localName) const;
    TopLevelLookup lookupTopLevelName(const std::string &localName) const;
    const std::unordered_map<std::string, TypeDecl> &types() const { return localTypes_; }
    const std::unordered_map<std::string, FunctionDecl> &functions() const {
        return localFunctions_;
    }
};

std::uint64_t hashModuleSource(const std::string &content);

}  // namespace lona
