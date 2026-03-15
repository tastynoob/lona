#include "session.hh"
#include "lona/err/err.hh"
#include "lona/scan/driver.hh"
#include "lona/type/scope.hh"
#include "lona/type/type.hh"
#include "lona/visitor.hh"
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/PassManager.h>
#include <llvm-18/llvm/IR/Verifier.h>
#include <llvm-18/llvm/Passes/OptimizationLevel.h>
#include <llvm-18/llvm/Passes/PassBuilder.h>
#include <llvm-18/llvm/Support/raw_ostream.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace lona {
namespace {

llvm::OptimizationLevel
getOptimizationLevel(int optLevel) {
    switch (optLevel) {
    case 0:
        return llvm::OptimizationLevel::O0;
    case 1:
        return llvm::OptimizationLevel::O1;
    case 2:
        return llvm::OptimizationLevel::O2;
    case 3:
        return llvm::OptimizationLevel::O3;
    default:
        return llvm::OptimizationLevel::O0;
    }
}

void
optimizeModule(llvm::Module &module, int optLevel) {
    if (optLevel <= 0) {
        return;
    }

    llvm::LoopAnalysisManager loopAM;
    llvm::FunctionAnalysisManager functionAM;
    llvm::CGSCCAnalysisManager cgsccAM;
    llvm::ModuleAnalysisManager moduleAM;
    llvm::PassBuilder passBuilder;

    passBuilder.registerLoopAnalyses(loopAM);
    passBuilder.registerFunctionAnalyses(functionAM);
    passBuilder.registerCGSCCAnalyses(cgsccAM);
    passBuilder.registerModuleAnalyses(moduleAM);
    passBuilder.crossRegisterProxies(loopAM, functionAM, cgsccAM, moduleAM);

    llvm::ModulePassManager modulePasses =
        passBuilder.buildPerModuleDefaultPipeline(getOptimizationLevel(optLevel));
    modulePasses.run(module, moduleAM);
}

bool
verifyCompiledModule(llvm::Module &module, std::ostream &out) {
    std::string verifyErrors;
    llvm::raw_string_ostream verifyOut(verifyErrors);
    if (!llvm::verifyModule(module, &verifyOut)) {
        return true;
    }
    verifyOut.flush();
    out << verifyErrors;
    return false;
}

}  // namespace

struct CompilerSession::CompilationUnit {
    llvm::LLVMContext context;
    llvm::Module module;
    llvm::IRBuilder<> builder;
    GlobalScope global;
    TypeTable types;

    explicit CompilationUnit(const SourceBuffer &source)
        : module(source.path(), context),
          builder(context),
          global(builder, module),
          types(module) {
        global.setTypeTable(&types);
    }
};

CompilerSession::CompilerSession() : diagnostics_(&sourceManager_) {}

const SourceBuffer &
CompilerSession::loadSource(const std::string &path) {
    return sourceManager_.loadFile(path);
}

AstNode *
CompilerSession::parseSource(const SourceBuffer &source) {
    std::istringstream input(source.content());
    Driver driver;
    driver.input(&input, source);
    return driver.parse();
}

int
CompilerSession::emitJson(AstNode *tree, std::ostream &out) const {
    Json root = Json::object();
    tree->toJson(root);
    out << root.dump(2) << std::endl;
    return 0;
}

int
CompilerSession::emitIR(const SourceBuffer &source, AstNode *tree,
                        const CompileOptions &options, std::ostream &out) const {
    CompilationUnit unit(source);

    scanningType(&unit.global, tree);
    compileModule(&unit.global, tree, options.debugInfo);
    optimizeModule(unit.module, options.optLevel);

    if (options.verifyIR && !verifyCompiledModule(unit.module, out)) {
        return 1;
    }

    std::string ir;
    llvm::raw_string_ostream irOut(ir);
    unit.module.print(irOut, nullptr);
    irOut.flush();
    out << ir;
    return 0;
}

int
CompilerSession::runFile(const std::string &inputPath,
                         const SessionOptions &options, std::ostream &out,
                         std::ostream &diag) {
    try {
        const auto &source = loadSource(inputPath);
        AstNode *tree = parseSource(source);
        if (tree == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Syntax,
                                  "I couldn't parse this file.");
        }

        if (options.outputMode == OutputMode::LLVMIR) {
            return emitIR(source, tree, options.compile, out);
        }
        return emitJson(tree, out);
    } catch (const DiagnosticError &error) {
        diagnostics_.emit(error, diag, inputPath);
        return 1;
    } catch (const std::exception &ex) {
        diagnostics_.emit(
            DiagnosticError(DiagnosticError::Category::Internal,
                            ex.what(),
                            "This looks like a compiler bug or infrastructure failure."),
            diag, inputPath);
        return 1;
    } catch (const char *ex) {
        diagnostics_.emit(
            DiagnosticError(DiagnosticError::Category::Internal,
                            ex,
                            "This looks like a compiler bug or infrastructure failure."),
            diag, inputPath);
        return 1;
    }
}

}  // namespace lona
