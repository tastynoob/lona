#include "lona/type/scope.hh"
#include "lona/type/type.hh"
#include <llvm-18/llvm/IR/GlobalIFunc.h>
#include <llvm-18/llvm/IR/IRBuilder.h>
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/Type.h>
#include <string>
#include <vector>

namespace lona {

/**
One project‘s directory structure:
src/
    package1/
        file1
        file2
    package2/
        file1
        file2
    main_file1
    main_file2
*/

class SourceFile {
    std::string path;

    llvm::Module* module;
    llvm::IRBuilder<>* builder;

    GlobalScope* global;
    TypeTable* typeMgr;

public:
    SourceFile(llvm::Module* module, std::string path) {
        this->module = module;
        this->builder = new llvm::IRBuilder<>(module->getContext());

        this->path = path;

        global = new GlobalScope(*builder, *module);
        typeMgr = new TypeTable(*module);
        global->setTypeTable(typeMgr);
    }

    llvm::Module& getModule() const { return *module; }

    GlobalScope* scope() const { return global; }
    TypeTable* types() const { return typeMgr; }

    const std::string& where() const { return path; }
};

class Package {
    std::string name;
    std::string path;

    std::vector<Package*> packages;
    std::vector<SourceFile*> files;

public:
    Package(const std::string& directory_path) {
        this->path = directory_path;
        this->name =
            directory_path.substr(directory_path.find_last_of("/\\") + 1);
    }

    void addPackage(Package* pkg) { packages.push_back(pkg); }

    void addFile(SourceFile* file) { files.push_back(file); }

    const std::string& getName() const { return name; }
    const std::string& getPath() const { return path; }
};

}