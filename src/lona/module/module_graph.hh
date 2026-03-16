#pragma once

#include "compilation_unit.hh"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lona {

class SourceBuffer;

class ModuleGraph {
    struct ModuleRecord {
        std::unique_ptr<CompilationUnit> unit;
        std::vector<std::string> dependencies;
        bool root = false;

        explicit ModuleRecord(std::unique_ptr<CompilationUnit> unit)
            : unit(std::move(unit)) {}
    };

    std::unordered_map<std::string, std::unique_ptr<ModuleRecord>> records_;
    std::unordered_map<std::string, std::string> moduleNameToPath_;
    std::unordered_map<std::string, std::vector<std::string>> reverseDependencies_;
    std::vector<std::string> loadOrder_;
    std::string rootPath_;

    ModuleRecord &requireRecord(const std::string &path);
    const ModuleRecord &requireRecord(const std::string &path) const;

public:
    CompilationUnit &getOrCreate(const SourceBuffer &source);
    CompilationUnit *find(const std::string &path);
    const CompilationUnit *find(const std::string &path) const;
    CompilationUnit *findByModuleName(const std::string &moduleName);
    const CompilationUnit *findByModuleName(const std::string &moduleName) const;

    void markRoot(const std::string &path);
    CompilationUnit *root();
    const CompilationUnit *root() const;

    void resetDependencies(const std::string &path);
    void addDependency(const std::string &path, std::string dependencyPath);
    const std::vector<std::string> &dependenciesOf(const std::string &path) const;
    const std::vector<std::string> &dependentsOf(const std::string &path) const;
    std::vector<std::string> postOrderFrom(const std::string &path) const;
    const std::vector<std::string> &loadOrder() const { return loadOrder_; }
};

}  // namespace lona
