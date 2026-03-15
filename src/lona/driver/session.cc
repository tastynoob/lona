#include "session.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/resolve/resolve.hh"
#include "lona/scan/driver.hh"
#include "lona/sema/hir.hh"
#include "lona/type/scope.hh"
#include "lona/type/type.hh"
#include "lona/visitor.hh"
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/PassManager.h>
#include <llvm-18/llvm/IR/Verifier.h>
#include <llvm-18/llvm/Passes/OptimizationLevel.h>
#include <llvm-18/llvm/Passes/PassBuilder.h>
#include <llvm-18/llvm/Support/raw_ostream.h>
#include <memory>
#include <nlohmann/json.hpp>
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

AstNode *
requireSyntaxTree(const CompilationUnit &unit) {
    auto *tree = unit.syntaxTree();
    if (!tree) {
        throw DiagnosticError(DiagnosticError::Category::Internal,
                              "compilation unit `" + unit.path() +
                                  "` is missing its parsed syntax tree",
                              "Parse the unit before lowering or emission.");
    }
    return tree;
}

AstStatList *
requireTopLevelBody(const CompilationUnit &unit) {
    auto *tree = requireSyntaxTree(unit);
    if (auto *program = dynamic_cast<AstProgram *>(tree)) {
        return program->body;
    }
    if (auto *body = dynamic_cast<AstStatList *>(tree)) {
        return body;
    }
    throw DiagnosticError(DiagnosticError::Category::Internal,
                          "compilation unit `" + unit.path() +
                              "` does not have a top-level statement list",
                          "This looks like a parser/session integration bug.");
}

std::string
resolveImportPath(const CompilationUnit &unit, const AstImport &importNode) {
    namespace fs = std::filesystem;
    fs::path importPath(importNode.path);
    if (importPath.has_extension()) {
        throw DiagnosticError(
            DiagnosticError::Category::Syntax, importNode.loc,
            "import paths should omit the file suffix",
            "Write imports like `import path/to/file`, not `import path/to/file.lo`.");
    }
    importPath += ".lo";
    if (importPath.is_relative()) {
        importPath = fs::path(unit.path()).parent_path() / importPath;
    }
    return importPath.lexically_normal().string();
}

bool
isValidModuleName(const std::string &name) {
    if (name.empty()) {
        return false;
    }
    auto isHead = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    auto isBody = [&](char ch) {
        return isHead(ch) || (ch >= '0' && ch <= '9');
    };
    if (!isHead(name.front())) {
        return false;
    }
    for (char ch : name) {
        if (!isBody(ch)) {
            return false;
        }
    }
    return true;
}

void
appendHIRFunctions(HIRModule &target, HIRModule *source) {
    if (!source) {
        return;
    }
    for (auto *func : source->getFunctions()) {
        target.addFunction(func);
    }
}

bool
isAllowedImportedTopLevelNode(AstNode *node) {
    if (node == nullptr) {
        return true;
    }
    if (node->is<AstImport>() || node->is<AstStructDecl>() || node->is<AstFuncDecl>()) {
        return true;
    }
    auto *list = dynamic_cast<AstStatList *>(node);
    return list != nullptr && list->isEmpty();
}

}  // namespace

struct IRBuildState {
    llvm::LLVMContext context;
    llvm::Module module;
    llvm::IRBuilder<> builder;
    GlobalScope global;
    TypeTable types;

    explicit IRBuildState(const CompilationUnit &unit)
        : module(unit.path(), context),
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

CompilationUnit &
CompilerSession::loadUnit(const std::string &path) {
    const auto &source = loadSource(path);
    return moduleGraph_.getOrCreate(source);
}

CompilationUnit &
CompilerSession::loadRootUnit(const std::string &path) {
    auto &unit = loadUnit(path);
    moduleGraph_.markRoot(unit.path());
    return unit;
}

AstNode *
CompilerSession::parseUnit(CompilationUnit &unit) {
    if (unit.hasSyntaxTree()) {
        return unit.syntaxTree();
    }

    std::istringstream input(unit.source().content());
    Driver driver;
    driver.input(&input, unit.source());
    auto *tree = driver.parse();
    if (tree != nullptr) {
        unit.setSyntaxTree(tree);
    }
    return tree;
}

void
CompilerSession::discoverUnitDependencies(CompilationUnit &unit) {
    if (unit.dependenciesScanned()) {
        return;
    }

    auto *body = requireTopLevelBody(unit);
    for (auto *stmt : body->getBody()) {
        auto *importNode = dynamic_cast<AstImport *>(stmt);
        if (!importNode) {
            continue;
        }
        auto importPath = resolveImportPath(unit, *importNode);
        auto &dependencyUnit = loadUnit(importPath);
        if (!isValidModuleName(dependencyUnit.moduleName())) {
            throw DiagnosticError(
                DiagnosticError::Category::Semantic, importNode->loc,
                "imported module `" + dependencyUnit.path() +
                    "` cannot be referenced as `file.xxx` because `" +
                    dependencyUnit.moduleName() + "` is not a valid identifier",
                "Rename the file so its base name matches identifier syntax.");
        }
        moduleGraph_.addDependency(unit.path(), dependencyUnit.path());
    }
    unit.markDependenciesScanned();
}

void
CompilerSession::parseLoadedUnits() {
    std::size_t index = 0;
    while (index < moduleGraph_.loadOrder().size()) {
        auto *unit = moduleGraph_.find(moduleGraph_.loadOrder()[index]);
        if (unit == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "module graph load order references a missing unit",
                                  "This looks like a compiler module graph bug.");
        }
        auto parseStart = Clock::now();
        auto *tree = parseUnit(*unit);
        lastStats_.parseMs += elapsedMillis(parseStart, Clock::now());
        if (tree == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Syntax,
                                  "I couldn't parse this file.");
        }
        discoverUnitDependencies(*unit);
        ++index;
    }
}

void
CompilerSession::validateImportedUnit(const CompilationUnit &unit) const {
    const auto *root = moduleGraph_.root();
    if (root != nullptr && root->path() == unit.path()) {
        return;
    }

    auto *body = requireTopLevelBody(unit);
    for (auto *stmt : body->getBody()) {
        if (isAllowedImportedTopLevelNode(stmt)) {
            continue;
        }
        throw DiagnosticError(
            DiagnosticError::Category::Semantic, stmt->loc,
            "imported file `" + unit.path() +
                "` cannot contain top-level executable statements",
            "Move this statement into a function, or keep top-level execution only in the root file.");
    }
}

int
CompilerSession::emitJson(const CompilationUnit &unit, std::ostream &out) const {
    auto *tree = requireSyntaxTree(unit);
    Json root = Json::object();
    tree->toJson(root);
    out << root.dump(2) << std::endl;
    return 0;
}

int
CompilerSession::emitIR(const CompilationUnit &unit,
                        const CompileOptions &options, std::ostream &out) {
    IRBuildState build(unit);
    HIRModule programHIR;
    const auto *root = moduleGraph_.root();

    auto declStart = Clock::now();
    for (auto it = moduleGraph_.loadOrder().rbegin();
         it != moduleGraph_.loadOrder().rend(); ++it) {
        auto *loadedUnit = moduleGraph_.find(*it);
        if (loadedUnit == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "module graph load order references a missing unit",
                                  "This looks like a compiler module graph bug.");
        }
        validateImportedUnit(*loadedUnit);
        collectUnitDeclarations(&build.global, *loadedUnit,
                                root && root->path() != loadedUnit->path());
    }
    lastStats_.declarationMs += elapsedMillis(declStart, Clock::now());

    auto lowerStart = Clock::now();
    for (const auto &path : moduleGraph_.loadOrder()) {
        auto *loadedUnit = moduleGraph_.find(path);
        if (loadedUnit == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "module graph load order references a missing unit",
                                  "This looks like a compiler module graph bug.");
        }
        auto resolved = resolveModule(&build.global, loadedUnit->syntaxTree(), loadedUnit);
        appendHIRFunctions(programHIR,
                           analyzeModule(&build.global, *resolved, loadedUnit));
    }
    lastStats_.lowerMs += elapsedMillis(lowerStart, Clock::now());

    auto codegenStart = Clock::now();
    emitHIRModule(&build.global, &programHIR, options.debugInfo, unit.path());
    lastStats_.codegenMs += elapsedMillis(codegenStart, Clock::now());

    auto optimizeStart = Clock::now();
    optimizeModule(build.module, options.optLevel);
    lastStats_.optimizeMs += elapsedMillis(optimizeStart, Clock::now());

    if (options.verifyIR) {
        auto verifyStart = Clock::now();
        bool verifyOk = verifyCompiledModule(build.module, out);
        lastStats_.verifyMs += elapsedMillis(verifyStart, Clock::now());
        if (!verifyOk) {
            return 1;
        }
    }

    std::string ir;
    llvm::raw_string_ostream irOut(ir);
    build.module.print(irOut, nullptr);
    irOut.flush();
    out << ir;
    return 0;
}

void
CompilerSession::printStats(std::ostream &out) const {
    auto oldFlags = out.flags();
    auto oldPrecision = out.precision();
    out << std::fixed << std::setprecision(3);
    out << "compile stats:\n";
    out << "  loaded-units: " << lastStats_.loadedUnits << '\n';
    out << "  parse-ms: " << lastStats_.parseMs << '\n';
    out << "  declarations-ms: " << lastStats_.declarationMs << '\n';
    out << "  lower-ms: " << lastStats_.lowerMs << '\n';
    out << "  codegen-ms: " << lastStats_.codegenMs << '\n';
    out << "  optimize-ms: " << lastStats_.optimizeMs << '\n';
    out << "  verify-ms: " << lastStats_.verifyMs << '\n';
    out << "  total-ms: " << lastStats_.totalMs << '\n';
    out.flags(oldFlags);
    out.precision(oldPrecision);
}

int
CompilerSession::runFile(const std::string &inputPath,
                         const SessionOptions &options, std::ostream &out,
                         std::ostream &diag) {
    lastStats_ = {};
    auto totalStart = Clock::now();
    auto finish = [&](int exitCode) {
        lastStats_.loadedUnits = moduleGraph_.loadOrder().size();
        lastStats_.totalMs = elapsedMillis(totalStart, Clock::now());
        return exitCode;
    };

    try {
        auto &unit = loadRootUnit(inputPath);
        parseLoadedUnits();
        AstNode *tree = unit.syntaxTree();
        if (tree == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Syntax,
                                  "I couldn't parse this file.");
        }

        if (options.outputMode == OutputMode::LLVMIR) {
            return finish(emitIR(unit, options.compile, out));
        }
        return finish(emitJson(unit, out));
    } catch (const DiagnosticError &error) {
        diagnostics_.emit(error, diag, inputPath);
        return finish(1);
    } catch (const std::exception &ex) {
        diagnostics_.emit(
            DiagnosticError(DiagnosticError::Category::Internal,
                            ex.what(),
                            "This looks like a compiler bug or infrastructure failure."),
            diag, inputPath);
        return finish(1);
    } catch (const char *ex) {
        diagnostics_.emit(
            DiagnosticError(DiagnosticError::Category::Internal,
                            ex,
                            "This looks like a compiler bug or infrastructure failure."),
            diag, inputPath);
        return finish(1);
    }
}

}  // namespace lona
