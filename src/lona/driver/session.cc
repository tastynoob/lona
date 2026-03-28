#include "session.hh"
#include "lona/ast/astnode.hh"
#include "lona/err/err.hh"
#include "lona/util/time.hh"
#include <iomanip>
#include <nlohmann/json.hpp>

namespace lona {

CompilerSession::CompilerSession()
    : loader_(workspace_),
      builder_(workspace_, loader_) {}

CompilerSession::~CompilerSession() = default;

int
CompilerSession::runEntry(const SessionOptions &options, std::ostream &out,
                          std::ostream &diag) {
    lastStats_ = {};
    auto totalStart = Clock::now();
    auto finish = [&](int exitCode) {
        lastStats_.loadedUnits = 0;
        lastStats_.totalMs = elapsedMillis(totalStart, Clock::now());
        return exitCode;
    };

    try {
        if (options.outputMode != OutputMode::EntryObject) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "entry emission was invoked with the wrong output mode",
                                  "This looks like a driver/session integration bug.");
        }
        return finish(builder_.emitHostedEntryObject(options.compile, lastStats_, out));
    } catch (const DiagnosticError &error) {
        diagnostics().emit(error, diag);
        return finish(1);
    } catch (const std::exception &ex) {
        diagnostics().emit(
            DiagnosticError(DiagnosticError::Category::Internal,
                            ex.what(),
                            "This looks like a compiler bug or infrastructure failure."),
            diag);
        return finish(1);
    } catch (const char *ex) {
        diagnostics().emit(
            DiagnosticError(DiagnosticError::Category::Internal,
                            ex,
                            "This looks like a compiler bug or infrastructure failure."),
            diag);
        return finish(1);
    }
}

void
CompilerSession::printStats(std::ostream &out) const {
    auto oldFlags = out.flags();
    auto oldPrecision = out.precision();
    const double cacheMs = lastStats_.cacheLookupMs + lastStats_.cacheRestoreMs;
    out << std::fixed << std::setprecision(3);
    out << "compile stats:\n";
    out << "  units:\n";
    out << "    loaded-units: " << lastStats_.loadedUnits << '\n';
    out << "    compiled-modules: " << lastStats_.compiledModules << '\n';
    out << "    reused-modules: " << lastStats_.reusedModules << '\n';
    out << "    emitted-module-bitcode: " << lastStats_.emittedModuleBitcode << '\n';
    out << "    reused-module-bitcode: " << lastStats_.reusedModuleBitcode << '\n';
    out << "    emitted-module-objects: " << lastStats_.emittedModuleObjects << '\n';
    out << "    reused-module-objects: " << lastStats_.reusedModuleObjects << '\n';
    out << "  timing-ms:\n";
    out << "    total-ms: " << lastStats_.totalMs << '\n';
    out << "    parse-ms: " << lastStats_.parseMs << '\n';
    out << "    dependency-scan-ms: " << lastStats_.dependencyScanMs << '\n';
    out << "    declarations-ms: " << lastStats_.declarationMs << '\n';
    out << "      dependency-declarations-ms: "
        << lastStats_.dependencyDeclarationMs << '\n';
    out << "      entry-declarations-ms: " << lastStats_.entryDeclarationMs << '\n';
    out << "    lower-ms: " << lastStats_.lowerMs << '\n';
    out << "      resolve-ms: " << lastStats_.resolveMs << '\n';
    out << "      analyze-ms: " << lastStats_.analyzeMs << '\n';
    out << "    codegen-ms: " << lastStats_.codegenMs << '\n';
    out << "      emit-llvm-ms: " << lastStats_.emitLlvmMs << '\n';
    out << "      output-emit-ms: " << lastStats_.outputEmitMs << '\n';
    out << "    cache-ms: " << cacheMs << '\n';
    out << "      cache-lookup-ms: " << lastStats_.cacheLookupMs << '\n';
    out << "      cache-restore-ms: " << lastStats_.cacheRestoreMs << '\n';
    out << "    optimize-ms: " << lastStats_.optimizeMs << '\n';
    out << "      module-optimize-ms: " << lastStats_.moduleOptimizeMs << '\n';
    out << "      lto-optimize-ms: " << lastStats_.ltoOptimizeMs << '\n';
    out << "    verify-ms: " << lastStats_.verifyMs << '\n';
    out << "      module-verify-ms: " << lastStats_.moduleVerifyMs << '\n';
    out << "      link-verify-ms: " << lastStats_.linkVerifyMs << '\n';
    out << "    link-ms: " << lastStats_.linkMs << '\n';
    out << "      link-load-ms: " << lastStats_.linkLoadMs << '\n';
    out << "      link-merge-ms: " << lastStats_.linkMergeMs << '\n';
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
        lastStats_.loadedUnits = builder_.loadedUnitCount();
        lastStats_.totalMs = elapsedMillis(totalStart, Clock::now());
        return exitCode;
    };

    try {
        auto &unit = loader_.loadRootUnit(inputPath);
        loader_.loadTransitiveUnits([this](
                                        const CompilationUnit &,
                                        double parseMs,
                                        double dependencyScanMs) {
            lastStats_.parseMs += parseMs;
            lastStats_.dependencyScanMs += dependencyScanMs;
        });
        AstNode *tree = unit.syntaxTree();
        if (tree == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Syntax,
                                  "I couldn't parse this file.");
        }

        if (options.outputMode == OutputMode::LLVMIR) {
            return finish(builder_.emitIR(unit, options.compile, lastStats_, out));
        }
        if (options.outputMode == OutputMode::EntryObject) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  "entry object emission should not load source files",
                                  "Use CompilerSession::runEntry instead.");
        }
        if (options.outputMode == OutputMode::ObjectFile) {
            return finish(builder_.emitObject(unit, options.compile, lastStats_, out));
        }
        if (options.outputMode == OutputMode::ObjectBundle) {
            return finish(builder_.emitObjectBundle(
                unit, options.compile, options.outputPath, options.cacheOutputPath,
                lastStats_, out));
        }
        auto *jsonTree = unit.requireSyntaxTree();
        Json root = Json::object();
        jsonTree->toJson(root);
        out << root.dump(2) << std::endl;
        return finish(0);
    } catch (const DiagnosticError &error) {
        diagnostics().emit(error, diag, inputPath);
        return finish(1);
    } catch (const std::exception &ex) {
        diagnostics().emit(
            DiagnosticError(DiagnosticError::Category::Internal,
                            ex.what(),
                            "This looks like a compiler bug or infrastructure failure."),
            diag, inputPath);
        return finish(1);
    } catch (const char *ex) {
        diagnostics().emit(
            DiagnosticError(DiagnosticError::Category::Internal,
                            ex,
                            "This looks like a compiler bug or infrastructure failure."),
            diag, inputPath);
        return finish(1);
    }
}

}  // namespace lona
