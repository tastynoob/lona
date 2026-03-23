#include "session.hh"
#include "lona/ast/astnode.hh"
#include "lona/err/err.hh"
#include <chrono>
#include <iomanip>
#include <nlohmann/json.hpp>

namespace lona {
namespace {
using Clock = std::chrono::steady_clock;

double
elapsedMillis(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
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

}  // namespace

CompilerSession::CompilerSession()
    : loader_(workspace_),
      builder_(workspace_, loader_) {}

CompilerSession::~CompilerSession() = default;

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
    out << "  link-ms: " << lastStats_.linkMs << '\n';
    out << "  compiled-modules: " << lastStats_.compiledModules << '\n';
    out << "  reused-modules: " << lastStats_.reusedModules << '\n';
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
        lastStats_.loadedUnits = builder_.loadedUnitCount();
        lastStats_.totalMs = elapsedMillis(totalStart, Clock::now());
        return exitCode;
    };

    try {
        auto &unit = loader_.loadRootUnit(inputPath);
        loader_.loadTransitiveUnits([this](const CompilationUnit &, double parseMs) {
            lastStats_.parseMs += parseMs;
        });
        AstNode *tree = unit.syntaxTree();
        if (tree == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Syntax,
                                  "I couldn't parse this file.");
        }

        if (options.outputMode == OutputMode::LLVMIR) {
            return finish(builder_.emitIR(unit, options.compile, lastStats_, out));
        }
        if (options.outputMode == OutputMode::ObjectFile) {
            return finish(builder_.emitObject(unit, options.compile, lastStats_, out));
        }
        auto *jsonTree = requireSyntaxTree(unit);
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
