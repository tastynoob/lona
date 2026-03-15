#pragma once

#include "lona/ast/astnode.hh"
#include "lona/source/source_manager.hh"
#include <string>
#include <unordered_map>

namespace lona {

class TypeClass;
class TypeTable;

enum class CompilationUnitStage {
    Loaded,
    Parsed,
    DependenciesScanned,
};

class CompilationUnit {
    std::string path_;
    std::string moduleName_;
    const SourceBuffer *source_ = nullptr;
    AstNode *syntaxTree_ = nullptr;
    CompilationUnitStage stage_ = CompilationUnitStage::Loaded;
    std::unordered_map<std::string, std::string> localTypeNames_;
    std::unordered_map<std::string, std::string> localFunctionNames_;
    mutable std::unordered_map<const TypeNode *, TypeClass *> resolvedTypes_;

public:
    explicit CompilationUnit(const SourceBuffer &source);

    const std::string &path() const { return path_; }
    const std::string &moduleName() const { return moduleName_; }
    const SourceBuffer &source() const;
    CompilationUnitStage stage() const { return stage_; }
    bool hasSyntaxTree() const { return syntaxTree_ != nullptr; }
    bool dependenciesScanned() const {
        return stage_ == CompilationUnitStage::DependenciesScanned;
    }
    AstNode *syntaxTree() const { return syntaxTree_; }

    void refreshSource(const SourceBuffer &source);
    void setSyntaxTree(AstNode *tree);
    void markDependenciesScanned();

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
