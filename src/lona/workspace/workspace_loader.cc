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
                          "compilation unit `" + unit.path() +
                              "` does not have a top-level statement list",
                          "This looks like a parser/session integration bug.");
}

std::string
resolveWorkspaceImportPath(const CompilationUnit &unit,
                           const AstImport &importNode) {
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
isValidWorkspaceModuleName(const std::string &name) {
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
    if (unit.dependenciesScanned()) {
        return;
    }

    workspace_.moduleGraph().resetDependencies(unit.path());
    unit.clearImportedModules();
    auto *body = requireWorkspaceTopLevelBody(unit);
    for (auto *stmt : body->getBody()) {
        auto *importNode = dynamic_cast<AstImport *>(stmt);
        if (!importNode) {
            continue;
        }
        auto importPath = resolveWorkspaceImportPath(unit, *importNode);
        auto &dependencyUnit = workspace_.loadUnit(importPath);
        if (!isValidWorkspaceModuleName(dependencyUnit.moduleName())) {
            throw DiagnosticError(
                DiagnosticError::Category::Semantic, importNode->loc,
                "imported module `" + dependencyUnit.path() +
                    "` cannot be referenced as `file.xxx` because `" +
                    dependencyUnit.moduleName() + "` is not a valid identifier",
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

    std::vector<std::string> pending = {root->path()};
    std::unordered_set<std::string> queued = {root->path()};
    for (std::size_t index = 0; index < pending.size(); ++index) {
        auto &loadedUnit = workspace_.loadUnit(pending[index]);
        auto parseStart = Clock::now();
        auto *tree = parseUnit(loadedUnit);
        auto parseMs = elapsedMillis(parseStart, Clock::now());
        if (observer) {
            observer(loadedUnit, parseMs);
        }
        if (tree == nullptr) {
            throw DiagnosticError(DiagnosticError::Category::Syntax,
                                  "I couldn't parse this file.");
        }
        discoverUnitDependencies(loadedUnit);
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
            "imported file `" + unit.path() +
                "` cannot contain top-level executable statements",
            "Move this statement into a function, or keep top-level execution only in the root file.");
    }
}

}  // namespace lona
