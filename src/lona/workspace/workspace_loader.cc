#include "workspace_loader.hh"
#include "lona/err/err.hh"
#include "lona/scan/driver.hh"
#include "lona/util/time.hh"
#include <filesystem>
#include <sstream>
#include <unordered_set>

namespace lona {

AstStatList *
requireWorkspaceTopLevelBody(const CompilationUnit &unit) {
    auto *tree = unit.requireSyntaxTree();
    if (auto *program = dynamic_cast<AstProgram *>(tree)) {
        return program->body;
    }
    if (auto *body = dynamic_cast<AstStatList *>(tree)) {
        return body;
    }
    throw DiagnosticError(DiagnosticError::Category::Internal,
                          "compilation unit `" + toStdString(unit.path()) +
                              "` does not have a top-level statement list",
                          "This looks like a parser/session integration bug.");
}

std::string
resolveWorkspaceImportPath(const CompilationUnit &unit,
                           const AstImport &importNode,
                           const std::vector<std::string> &includePaths) {
    namespace fs = std::filesystem;
    fs::path importPath(importNode.path);
    if (importPath.has_extension()) {
        throw DiagnosticError(
            DiagnosticError::Category::Syntax, importNode.loc,
            "import paths should omit the file suffix",
            "Write imports like `import path/to/file`, not `import path/to/file.lo`.");
    }
    importPath += ".lo";
    if (!importPath.is_relative()) {
        return importPath.lexically_normal().string();
    }

    std::vector<fs::path> candidates;
    candidates.reserve(includePaths.size() + 1);
    candidates.push_back(
        fs::path(toStdString(unit.path())).parent_path() / importPath);
    for (const auto &includePath : includePaths) {
        candidates.push_back(fs::path(includePath) / importPath);
    }

    for (const auto &candidate : candidates) {
        auto normalized = candidate.lexically_normal();
        std::error_code error;
        if (fs::exists(normalized, error)) {
            return normalized.string();
        }
        if (error) {
            auto searchedPath = normalized.string();
            auto includeRoot = normalized.parent_path().lexically_normal().string();
            throw DiagnosticError(
                DiagnosticError::Category::Driver, importNode.loc,
                "I couldn't inspect include path `" + includeRoot +
                    "` while resolving import `" + importNode.path + "`.",
                "Check that the include directory exists and that you have search permission for `" +
                    searchedPath + "`.");
        }
    }

    return candidates.front().lexically_normal().string();
}

bool
isValidWorkspaceModuleName(const string &name) {
    auto view = name.view();
    if (view.empty()) {
        return false;
    }
    auto isHead = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    auto isBody = [&](char ch) {
        return isHead(ch) || (ch >= '0' && ch <= '9');
    };
    if (!isHead(view.front())) {
        return false;
    }
    for (char ch : view) {
        if (!isBody(ch)) {
            return false;
        }
    }
    return true;
}

bool
isAllowedWorkspaceImportedTopLevelNode(AstNode *node) {
    if (node == nullptr) {
        return true;
    }
    if (node->is<AstImport>() || node->is<AstStructDecl>() || node->is<AstFuncDecl>()) {
        return true;
    }
    auto *list = dynamic_cast<AstStatList *>(node);
    return list != nullptr && list->isEmpty();
}

void
WorkspaceLoader::setIncludePaths(std::vector<std::string> includePaths) {
    includePaths_.clear();
    includePaths_.reserve(includePaths.size());
    for (auto &includePath : includePaths) {
        if (includePath.empty()) {
            continue;
        }
        includePaths_.push_back(
            std::filesystem::path(std::move(includePath)).lexically_normal().string());
    }
}

CompilationUnit &
WorkspaceLoader::loadRootUnit(const std::string &path) const {
    return workspace_.loadRootUnit(path);
}

AstNode *
WorkspaceLoader::parseUnit(CompilationUnit &unit) const {
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
WorkspaceLoader::discoverUnitDependencies(CompilationUnit &unit) const {
    workspace_.moduleGraph().resetDependencies(unit.path());
    unit.clearImportedModules();
    auto *body = requireWorkspaceTopLevelBody(unit);
    for (auto *stmt : body->getBody()) {
        auto *importNode = dynamic_cast<AstImport *>(stmt);
        if (!importNode) {
            continue;
        }
        auto importPath =
            resolveWorkspaceImportPath(unit, *importNode, includePaths_);
        auto &dependencyUnit = workspace_.loadUnit(importPath);
        if (!isValidWorkspaceModuleName(dependencyUnit.moduleName())) {
            throw DiagnosticError(
                DiagnosticError::Category::Semantic, importNode->loc,
                "imported module `" + toStdString(dependencyUnit.path()) +
                    "` cannot be referenced as `file.xxx` because `" +
                    toStdString(dependencyUnit.moduleName()) +
                    "` is not a valid identifier",
                "Rename the file so its base name matches identifier syntax.");
        }
        workspace_.moduleGraph().addDependency(unit.path(), dependencyUnit.path());
        unit.addImportedModule(dependencyUnit.moduleName(), dependencyUnit);
    }
    unit.markDependenciesScanned();
}

void
WorkspaceLoader::loadTransitiveUnits(ParseObserver observer) const {
    auto *root = workspace_.moduleGraph().root();
    if (root == nullptr) {
        return;
    }

    std::vector<string> pending = {root->path()};
    std::unordered_set<string> queued = {root->path()};
    for (std::size_t index = 0; index < pending.size(); ++index) {
        auto &loadedUnit = workspace_.loadUnit(pending[index]);
        auto parseStart = Clock::now();
        auto *tree = parseUnit(loadedUnit);
        auto parseMs = elapsedMillis(parseStart, Clock::now());
        if (observer) {
            observer(loadedUnit, parseMs, 0.0);
        }
        if (tree == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Syntax,
                                  "I couldn't parse this file.");
        }
        auto dependencyScanStart = Clock::now();
        discoverUnitDependencies(loadedUnit);
        auto dependencyScanMs =
            elapsedMillis(dependencyScanStart, Clock::now());
        if (observer) {
            observer(loadedUnit, 0.0, dependencyScanMs);
        }
        for (const auto &dependencyPath :
             workspace_.moduleGraph().dependenciesOf(loadedUnit.path())) {
            if (queued.emplace(dependencyPath).second) {
                pending.push_back(dependencyPath);
            }
        }
    }
}

void
WorkspaceLoader::validateImportedUnit(const CompilationUnit &unit) const {
    const auto *root = workspace_.moduleGraph().root();
    if (root != nullptr && root->path() == unit.path()) {
        return;
    }

    auto *body = requireWorkspaceTopLevelBody(unit);
    for (auto *stmt : body->getBody()) {
        if (isAllowedWorkspaceImportedTopLevelNode(stmt)) {
            continue;
        }
        throw DiagnosticError(
            DiagnosticError::Category::Semantic, stmt->loc,
            "imported file `" + toStdString(unit.path()) +
                "` cannot contain top-level executable statements",
            "Move this statement into a function, or keep top-level execution only in the root file.");
    }
}

}  // namespace lona
