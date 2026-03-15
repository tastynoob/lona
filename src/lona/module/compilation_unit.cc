#include "compilation_unit.hh"
#include "lona/type/type.hh"
#include <cassert>
#include <filesystem>
#include <utility>

namespace lona {
namespace {

std::string
deriveModuleName(const std::string &path) {
    namespace fs = std::filesystem;
    auto stem = fs::path(path).stem().string();
    if (!stem.empty()) {
        return stem;
    }
    return fs::path(path).filename().string();
}

TypeClass *
resolveTypeNode(TypeTable *typeTable, const CompilationUnit &unit, TypeNode *node) {
    if (!typeTable || !node) {
        return nullptr;
    }

    if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
        auto rawName = std::string(base->name.tochara(), base->name.size());
        if (rawName.find('.') == std::string::npos) {
            if (const auto *resolved = unit.findLocalType(rawName)) {
                return typeTable->getType(llvm::StringRef(*resolved));
            }
        }
        return typeTable->getType(base->name);
    }

    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        auto *type = resolveTypeNode(typeTable, unit, pointer->base);
        for (uint32_t i = 0; type && i < pointer->dim; ++i) {
            type = typeTable->createPointerType(type);
        }
        return type;
    }

    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        auto *elementType = resolveTypeNode(typeTable, unit, array->base);
        if (!elementType) {
            return nullptr;
        }
        return typeTable->createArrayType(elementType, array->dim);
    }

    if (auto *func = dynamic_cast<FuncTypeNode *>(node)) {
        std::vector<TypeClass *> argTypes;
        argTypes.reserve(func->args.size());
        for (auto *arg : func->args) {
            auto *argType = resolveTypeNode(typeTable, unit, arg);
            if (!argType) {
                return nullptr;
            }
            argTypes.push_back(argType);
        }
        auto *retType = resolveTypeNode(typeTable, unit, func->ret);
        return typeTable->getOrCreateFunctionType(argTypes, retType);
    }

    return nullptr;
}

}  // namespace

CompilationUnit::CompilationUnit(const SourceBuffer &source) {
    refreshSource(source);
}

const SourceBuffer &
CompilationUnit::source() const {
    assert(source_ != nullptr);
    return *source_;
}

void
CompilationUnit::refreshSource(const SourceBuffer &source) {
    path_ = source.path();
    moduleName_ = deriveModuleName(path_);
    source_ = &source;
    clearInterface();
}

void
CompilationUnit::setSyntaxTree(AstNode *tree) {
    syntaxTree_ = tree;
    stage_ = tree ? CompilationUnitStage::Parsed : CompilationUnitStage::Loaded;
}

void
CompilationUnit::markDependenciesScanned() {
    if (syntaxTree_ != nullptr) {
        stage_ = CompilationUnitStage::DependenciesScanned;
    }
}

void
CompilationUnit::clearInterface() {
    localTypeNames_.clear();
    localFunctionNames_.clear();
}

bool
CompilationUnit::bindLocalType(std::string localName, std::string resolvedName) {
    return localTypeNames_
        .emplace(std::move(localName), std::move(resolvedName))
        .second;
}

bool
CompilationUnit::bindLocalFunction(std::string localName, std::string resolvedName) {
    return localFunctionNames_
        .emplace(std::move(localName), std::move(resolvedName))
        .second;
}

const std::string *
CompilationUnit::findLocalType(const std::string &localName) const {
    auto found = localTypeNames_.find(localName);
    if (found == localTypeNames_.end()) {
        return nullptr;
    }
    return &found->second;
}

const std::string *
CompilationUnit::findLocalFunction(const std::string &localName) const {
    auto found = localFunctionNames_.find(localName);
    if (found == localFunctionNames_.end()) {
        return nullptr;
    }
    return &found->second;
}

TypeClass *
CompilationUnit::resolveType(TypeTable *typeTable, TypeNode *node) const {
    return resolveTypeNode(typeTable, *this, node);
}

}  // namespace lona
