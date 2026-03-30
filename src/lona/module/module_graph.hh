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
        std::vector<string> dependencies;
        bool root = false;

        explicit ModuleRecord(std::unique_ptr<CompilationUnit> unit)
            : unit(std::move(unit)) {}
    };

    std::unordered_map<string, std::unique_ptr<ModuleRecord>> records_;
    std::unordered_map<string, string> moduleNameToPath_;
    std::unordered_map<string, std::vector<string>> reverseDependencies_;
    std::vector<string> loadOrder_;
    string rootPath_;

    ModuleRecord &requireRecord(const string &path);
    ModuleRecord &requireRecord(const std::string &path) {
        return requireRecord(string(path));
    }
    const ModuleRecord &requireRecord(const string &path) const;
    const ModuleRecord &requireRecord(const std::string &path) const {
        return requireRecord(string(path));
    }

public:
    CompilationUnit &getOrCreate(const SourceBuffer &source);
    CompilationUnit *find(const string &path);
    CompilationUnit *find(const std::string &path) {
        return find(string(path));
    }
    const CompilationUnit *find(const string &path) const;
    const CompilationUnit *find(const std::string &path) const {
        return find(string(path));
    }
    CompilationUnit *findByModuleName(const string &moduleName);
    CompilationUnit *findByModuleName(const std::string &moduleName) {
        return findByModuleName(string(moduleName));
    }
    const CompilationUnit *findByModuleName(const string &moduleName) const;
    const CompilationUnit *findByModuleName(
        const std::string &moduleName) const {
        return findByModuleName(string(moduleName));
    }

    void markRoot(const string &path);
    void markRoot(const std::string &path) { markRoot(string(path)); }
    CompilationUnit *root();
    const CompilationUnit *root() const;

    void resetDependencies(const string &path);
    void resetDependencies(const std::string &path) {
        resetDependencies(string(path));
    }
    void addDependency(const string &path, string dependencyPath);
    void addDependency(const std::string &path, std::string dependencyPath) {
        addDependency(string(path), string(std::move(dependencyPath)));
    }
    const std::vector<string> &dependenciesOf(const string &path) const;
    const std::vector<string> &dependenciesOf(const std::string &path) const {
        return dependenciesOf(string(path));
    }
    const std::vector<string> &dependentsOf(const string &path) const;
    const std::vector<string> &dependentsOf(const std::string &path) const {
        return dependentsOf(string(path));
    }
    std::vector<string> postOrderFrom(const string &path) const;
    std::vector<string> postOrderFrom(const std::string &path) const {
        return postOrderFrom(string(path));
    }
    const std::vector<string> &loadOrder() const { return loadOrder_; }
};

}  // namespace lona
