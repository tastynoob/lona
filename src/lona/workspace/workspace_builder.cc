#include "workspace_builder.hh"
#include "lona/err/err.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/hir.hh"
#include "lona/visitor.hh"
#include <chrono>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/PassManager.h>
#include <llvm-18/llvm/IR/Verifier.h>
#include <llvm-18/llvm/Passes/OptimizationLevel.h>
#include <llvm-18/llvm/Passes/PassBuilder.h>
#include <sstream>
#include <utility>

namespace lona {
namespace {

using Clock = std::chrono::steady_clock;

double
elapsedMillis(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
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

std::unique_ptr<llvm::Module>
parseArtifactModule(const ModuleArtifact &artifact, llvm::LLVMContext &context) {
    llvm::SMDiagnostic error;
    auto module = llvm::parseAssemblyString(artifact.llvmIR(), error, context);
    if (module) {
        return module;
    }

    std::string render;
    llvm::raw_string_ostream renderOut(render);
    error.print(artifact.path().c_str(), renderOut);
    renderOut.flush();
    throw DiagnosticError(DiagnosticError::Category::Internal,
                          "failed to parse cached LLVM IR for module `" +
                              artifact.path() + "`",
                          render);
}

void
appendHIRFunctions(HIRModule &target, const HIRModule &source) {
    for (auto *func : source.getFunctions()) {
        target.addFunction(func);
    }
}

}  // namespace

WorkspaceBuilder::WorkspaceBuilder(CompilerWorkspace &workspace,
                                   const WorkspaceLoader &loader)
    : workspace_(workspace),
      loader_(loader),
      executor_(createSerialModuleExecutor()) {
    pipeline_.addStage("collect-declarations", [this](IRPipelineContext &context) {
        auto start = Clock::now();
        const bool exportEntryNamespace =
            context.rootUnit && context.rootUnit->path() != context.entryUnit.path();
        for (const auto &dependencyPath :
             context.moduleGraph.dependenciesOf(context.entryUnit.path())) {
            auto *loadedUnit = workspace_.moduleGraph().find(dependencyPath);
            if (loadedUnit == nullptr) {
                throw DiagnosticError(DiagnosticError::Category::Internal,
                                      "module graph dependency references a missing unit",
                                      "This looks like a compiler module graph bug.");
            }
            loader_.validateImportedUnit(*loadedUnit);
            collectUnitDeclarations(&context.build.global, *loadedUnit, true);
        }
        collectUnitDeclarations(&context.build.global, context.entryUnit,
                                exportEntryNamespace);
        context.stats.declarationMs += elapsedMillis(start, Clock::now());
        return 0;
    });

    pipeline_.addStage("lower-hir", [](IRPipelineContext &context) {
        auto start = Clock::now();
        auto resolved = resolveModule(&context.build.global, context.entryUnit.syntaxTree(),
                                      &context.entryUnit);
        auto hirModule =
            analyzeModule(&context.build.global, *resolved, &context.entryUnit);
        appendHIRFunctions(context.programHIR, *hirModule);
        context.loweredModules.push_back(std::move(hirModule));
        context.stats.lowerMs += elapsedMillis(start, Clock::now());
        return 0;
    });

    pipeline_.addStage("emit-llvm", [](IRPipelineContext &context) {
        auto start = Clock::now();
        emitHIRModule(&context.build.global, &context.programHIR,
                      context.options.debugInfo, context.entryUnit.path());
        context.stats.codegenMs += elapsedMillis(start, Clock::now());
        return 0;
    });

    pipeline_.addStage("optimize-llvm", [](IRPipelineContext &context) {
        auto start = Clock::now();
        optimizeModule(context.build.module, context.options.optLevel);
        context.stats.optimizeMs += elapsedMillis(start, Clock::now());
        return 0;
    });

    pipeline_.addStage("verify-llvm", [](IRPipelineContext &context) {
        if (!context.options.verifyIR) {
            return 0;
        }
        auto start = Clock::now();
        const bool verifyOk = verifyCompiledModule(context.build.module, context.out);
        context.stats.verifyMs += elapsedMillis(start, Clock::now());
        return verifyOk ? 0 : 1;
    });

    pipeline_.addStage("print-llvm", [](IRPipelineContext &context) {
        std::string ir;
        llvm::raw_string_ostream irOut(ir);
        context.build.module.print(irOut, nullptr);
        irOut.flush();
        context.out << ir;
        return 0;
    });
}

std::unordered_map<std::string, std::uint64_t>
WorkspaceBuilder::collectDependencyInterfaceHashes(const CompilationUnit &unit) const {
    std::unordered_map<std::string, std::uint64_t> hashes;
    for (const auto &dependencyPath : workspace_.moduleGraph().dependenciesOf(unit.path())) {
        auto *dependency = workspace_.moduleGraph().find(dependencyPath);
        if (dependency == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "module graph dependency references a missing unit",
                                  "This looks like a compiler module graph bug.");
        }
        hashes.emplace(dependency->moduleKey(), dependency->interfaceHash());
    }
    return hashes;
}

bool
WorkspaceBuilder::matchesArtifact(const CompilationUnit &unit,
                                  const ModuleArtifact &artifact) const {
    if (artifact.sourceHash() != unit.sourceHash() ||
        artifact.interfaceHash() != unit.interfaceHash() ||
        artifact.implementationHash() != unit.implementationHash()) {
        return false;
    }
    return artifact.dependencyInterfaceHashes() ==
        collectDependencyInterfaceHashes(unit);
}

const ModuleArtifact *
WorkspaceBuilder::reusableArtifactFor(const CompilationUnit &unit) const {
    auto *artifact = workspace_.findArtifact(unit.path());
    if (artifact == nullptr) {
        return nullptr;
    }
    return matchesArtifact(unit, *artifact) ? artifact : nullptr;
}

ModuleArtifact
WorkspaceBuilder::createArtifact(const CompilationUnit &unit) const {
    ModuleArtifact artifact(unit.path(), unit.moduleKey(), unit.moduleName(),
                            unit.sourceHash(), unit.interfaceHash(),
                            unit.implementationHash());
    artifact.setDependencyInterfaceHashes(collectDependencyInterfaceHashes(unit));
    return artifact;
}

int
WorkspaceBuilder::compileModule(CompilationUnit &unit, const CompileOptions &options,
                                ModuleArtifact &artifact, SessionStats &stats,
                                std::ostream &out) const {
    std::ostringstream ir;
    IRPipelineContext context(unit, workspace_.moduleGraph(), options, ir, stats);
    context.rootUnit = workspace_.moduleGraph().root();
    int exitCode = pipeline_.run(context);
    if (exitCode == 0) {
        unit.markCompiled();
        artifact.setLLVMIR(ir.str());
        ++stats.compiledModules;
    } else {
        out << ir.str();
    }
    return exitCode;
}

int
WorkspaceBuilder::linkArtifacts(const CompilationUnit &rootUnit, bool verifyIR,
                                std::ostream &out, double *linkMs,
                                double *verifyMs) const {
    auto *rootArtifact = workspace_.findArtifact(rootUnit.path());
    if (rootArtifact == nullptr) {
        throw DiagnosticError(DiagnosticError::Category::Internal,
                              "root module artifact was not produced",
                              "This looks like a compiler module scheduling bug.");
    }

    auto start = Clock::now();
    llvm::LLVMContext context;
    auto linkedModule = parseArtifactModule(*rootArtifact, context);
    llvm::Linker linker(*linkedModule);
    for (const auto &path : workspace_.moduleGraph().postOrderFrom(rootUnit.path())) {
        if (path == rootUnit.path()) {
            continue;
        }
        auto *artifact = workspace_.findArtifact(path);
        if (artifact == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "linked module is missing dependency artifact `" +
                                      path + "`",
                                  "This looks like a compiler module scheduling bug.");
        }
        auto dependencyModule = parseArtifactModule(*artifact, context);
        if (linker.linkInModule(std::move(dependencyModule))) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "failed to link module `" + artifact->path() +
                    "` into root module `" + rootUnit.path() + "`",
                "Check for duplicate IR symbols or incompatible LLVM module state.");
        }
    }

    if (verifyIR) {
        auto verifyStart = Clock::now();
        const bool ok = verifyCompiledModule(*linkedModule, out);
        if (verifyMs != nullptr) {
            *verifyMs += elapsedMillis(verifyStart, Clock::now());
        }
        if (!ok) {
            return 1;
        }
    }

    std::string linkedIR;
    llvm::raw_string_ostream irOut(linkedIR);
    linkedModule->print(irOut, nullptr);
    irOut.flush();
    if (linkMs != nullptr) {
        *linkMs += elapsedMillis(start, Clock::now());
    }
    out << linkedIR;
    return 0;
}

std::size_t
WorkspaceBuilder::loadedUnitCount() const {
    auto *root = workspace_.moduleGraph().root();
    return root ? workspace_.moduleGraph().postOrderFrom(root->path()).size() : 0;
}

int
WorkspaceBuilder::emitIR(CompilationUnit &rootUnit, const CompileOptions &options,
                         SessionStats &stats, std::ostream &out) const {
    workspace_.buildQueue().reset(workspace_.moduleGraph(), rootUnit.path());
    int exitCode =
        executor_->execute(workspace_.buildQueue(), [&](const std::string &path) -> int {
            auto *queuedUnit = workspace_.moduleGraph().find(path);
            if (queuedUnit == nullptr) {
                throw DiagnosticError(DiagnosticError::Category::Internal,
                                      "module build queue references a missing unit",
                                      "This looks like a compiler module queue bug.");
            }

            if (reusableArtifactFor(*queuedUnit) != nullptr) {
                queuedUnit->markCompiled();
                ++stats.reusedModules;
                return 0;
            }

            ModuleArtifact artifact = createArtifact(*queuedUnit);
            int moduleExitCode =
                compileModule(*queuedUnit, options, artifact, stats, out);
            if (moduleExitCode != 0) {
                return moduleExitCode;
            }
            workspace_.storeArtifact(std::move(artifact));
            return 0;
        });
    if (exitCode != 0) {
        return exitCode;
    }

    return linkArtifacts(rootUnit, options.verifyIR, out, &stats.linkMs,
                         &stats.verifyMs);
}

}  // namespace lona
