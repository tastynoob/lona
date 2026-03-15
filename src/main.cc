#include "cmdline.hpp"
#include "lona/ast/astnode.hh"
#include "lona/err/err.hh"
#include "lona/scan/driver.hh"
#include "lona/type/package.hh"
#include "lona/visitor.hh"
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/PassManager.h>
#include <llvm-18/llvm/IR/Verifier.h>
#include <llvm-18/llvm/Passes/OptimizationLevel.h>
#include <llvm-18/llvm/Passes/PassBuilder.h>
#include <llvm-18/llvm/Support/raw_ostream.h>
#include <string>

namespace {

struct CompileOptions {
    int optLevel = 0;
    bool verifyIR = false;
    bool debugInfo = false;
};

lona::AstNode *
parseInput(std::istream &input, const std::string &inputPath) {
    lona::Driver driver;
    driver.input(&input, inputPath);
    return driver.parse();
}

int
emitJson(lona::AstNode *tree, std::ostream &out) {
    Json root = Json::object();
    tree->toJson(root);
    out << root.dump(2) << std::endl;
    return 0;
}

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

[[noreturn]] void
finishProcess(int exitCode, std::ostream *out = nullptr) {
    if (out != nullptr) {
        out->flush();
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(exitCode);
}

int
emitIR(lona::AstNode *tree, const std::string &inputPath, const CompileOptions &options,
       std::ostream &out) {
    llvm::LLVMContext context;
    llvm::Module module(inputPath, context);
    lona::SourceFile source(&module, inputPath);

    lona::scanningType(source.scope(), tree);
    lona::compileModule(source.scope(), tree, options.debugInfo);
    optimizeModule(module, options.optLevel);

    if (options.verifyIR && !verifyCompiledModule(module, out)) {
        return 1;
    }

    std::string ir;
    llvm::raw_string_ostream irOut(ir);
    module.print(irOut, nullptr);
    irOut.flush();
    out << ir;
    return 0;
}

}  // namespace

int
main(int argc, char *argv[]) {
    cmdline::parser cli;
    cli.add("emit-ir", 'S', "print LLVM IR instead of AST JSON");
    cli.add("verify-ir", 0, "verify generated LLVM IR before printing");
    cli.add("debug", 'g', "emit LLVM debug metadata");
    cli.add<int>("opt", 'O', "LLVM optimization level (0-3)", false, 0,
                 cmdline::range(0, 3));
    cli.parse_check(argc, argv);

    const auto &args = cli.rest();
    if (args.empty() || args.size() > 2) {
        std::cerr << cli.usage();
        return 1;
    }

    const std::string &inputPath = args[0];
    std::ifstream input(inputPath);
    if (!input) {
        std::cerr << "Error: cannot open input file: " << inputPath << std::endl;
        return 1;
    }

    try {
        lona::AstNode *tree = parseInput(input, inputPath);
        if (tree == nullptr) {
            throw lona::DiagnosticError(
                lona::DiagnosticError::Category::Syntax,
                "I couldn't parse this file.");
        }

        std::ostream *out = &std::cout;
        std::ofstream output;
        if (args.size() == 2) {
            output.open(args[1]);
            if (!output) {
                std::cerr << "Error: cannot open output file: " << args[1]
                          << std::endl;
                return 1;
            }
            out = &output;
        }

        const bool compileMode = cli.exist("emit-ir") || cli.exist("verify-ir") ||
                                 cli.exist("debug") || cli.exist("opt");
        if (compileMode) {
            CompileOptions options;
            options.optLevel = cli.get<int>("opt");
            options.verifyIR = cli.exist("verify-ir");
            options.debugInfo = cli.exist("debug");
            finishProcess(emitIR(tree, inputPath, options, *out), out);
        }

        finishProcess(emitJson(tree, *out), out);
    } catch (const lona::DiagnosticError &error) {
        std::cerr << lona::formatDiagnostic(error, inputPath) << std::flush;
        finishProcess(1);
    } catch (const std::exception &ex) {
        std::cerr << "Error: " << ex.what() << std::endl << std::flush;
        finishProcess(1);
    } catch (const char *ex) {
        std::cerr << "Error: " << ex << std::endl << std::flush;
        finishProcess(1);
    }
}
