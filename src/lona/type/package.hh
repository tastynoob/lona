#include <llvm-18/llvm/IR/IRBuilder.h>
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/Type.h>
#include <string>
#include <vector>

namespace lona {

class SourceFile {
    std::string file_name;
    std::string directory_path;

    llvm::Module* module;
    llvm::IRBuilder<> *builder;

public:
    SourceFile(llvm::LLVMContext &context, const std::string &file_path) {
        this->module = new llvm::Module(file_path, context);
        this->builder = new llvm::IRBuilder<>(context);

        module->setDataLayout("e-m:e-i64:64-f80:128-n8:16:32:64-S128");
        module->setTargetTriple("x86_64-unknown-linux-gnu");

        this->file_name = file_path.substr(file_path.find_last_of("/\\") + 1);
        this->directory_path = file_path.substr(0, file_path.find_last_of("/\\"));
    }

    const std::string& where() const {
        return directory_path;
    }


};

class Package {
    std::string name;
    std::string path;

    std::vector<Package*> dependencies;
    std::vector<SourceFile*> files;
public:
    Package(const std::string & directory_path) {
        this->path = directory_path;
        this->name = directory_path.substr(directory_path.find_last_of("/\\") + 1);
    }

    void addDependency(Package *pkg) {
        dependencies.push_back(pkg);
    }

    void addFile(SourceFile *file) {
        files.push_back(file);
    }

    const std::string& getName() const { return name; }
    const std::string& getPath() const { return path; }

};



class Project {
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
    std::vector<Package*> packages;
    SourceFile* main_file;
};



}