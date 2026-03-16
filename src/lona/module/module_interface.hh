#pragma once

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
class AstNode;

class ModuleInterface {
public:
    struct TypeDecl {
        std::string localName;
        std::string exportedName;
        TypeClass *type = nullptr;
    };

    struct FunctionDecl {
        std::string localName;
        std::string exportedName;
        FuncType *type = nullptr;
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
    StructType *declareStructType(const std::string &localName);
    bool declareFunction(std::string localName, FuncType *type);
    PointerType *getOrCreatePointerType(TypeClass *pointeeType);
    ArrayType *getOrCreateArrayType(TypeClass *elementType,
                                    std::vector<AstNode *> dimensions = {});
    FuncType *getOrCreateFunctionType(const std::vector<TypeClass *> &argTypes,
                                      TypeClass *retType);
    const TypeDecl *findType(const std::string &localName) const;
    const FunctionDecl *findFunction(const std::string &localName) const;
    const std::unordered_map<std::string, TypeDecl> &types() const { return localTypes_; }
    const std::unordered_map<std::string, FunctionDecl> &functions() const {
        return localFunctions_;
    }
};

std::uint64_t hashModuleSource(const std::string &content);

}  // namespace lona
