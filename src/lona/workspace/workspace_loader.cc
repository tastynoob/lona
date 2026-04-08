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
defaultWorkspaceModulePath(const std::string &path) {
    auto normalized = std::filesystem::path(path).lexically_normal();
    normalized.replace_extension();
    auto modulePath = normalized.filename().string();
    if (!modulePath.empty()) {
        return modulePath;
    }
    return normalized.string();
}

std::string
describeModuleRoots(const std::vector<std::string> &roots) {
    if (roots.empty()) {
        return "<none>";
    }
    std::string text;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += "`" + roots[i] + "`";
    }
    return text;
}

std::filesystem::path
normalizeModuleFsPath(const std::filesystem::path &path) {
    namespace fs = std::filesystem;
    if (path.empty()) {
        return fs::current_path().lexically_normal();
    }
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return fs::absolute(path).lexically_normal();
}

bool
isPathUnderRoot(const std::filesystem::path &path,
                const std::filesystem::path &root) {
    if (root.empty()) {
        return false;
    }
    auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() &&
           relative.native().find("..") != 0;
}

std::vector<std::string>
buildWorkspaceModuleRoots(const std::string &rootPath,
                          const std::vector<std::string> &includePaths) {
    namespace fs = std::filesystem;

    auto normalizedRootPath = normalizeModuleFsPath(fs::path(rootPath));
    auto implicitRoot = normalizedRootPath.parent_path();
    std::vector<fs::path> normalizedRoots;
    normalizedRoots.reserve(includePaths.size() + 1);

    auto addUniqueRoot = [&](const fs::path &root) {
        if (root.empty()) {
            return;
        }
        auto normalized = normalizeModuleFsPath(root);
        for (const auto &existing : normalizedRoots) {
            if (existing == normalized) {
                return;
            }
        }
        normalizedRoots.push_back(normalized);
    };

    bool rootCoveredByExplicitRoot = false;
    for (const auto &includePath : includePaths) {
        auto includeRoot = normalizeModuleFsPath(fs::path(includePath));
        if (includeRoot.empty()) {
            continue;
        }
        if (isPathUnderRoot(normalizedRootPath, includeRoot)) {
            rootCoveredByExplicitRoot = true;
        }
        addUniqueRoot(includeRoot);
    }

    if (!rootCoveredByExplicitRoot) {
        addUniqueRoot(implicitRoot);
    }

    for (std::size_t i = 0; i < normalizedRoots.size(); ++i) {
        for (std::size_t j = i + 1; j < normalizedRoots.size(); ++j) {
            if (!isPathUnderRoot(normalizedRoots[i], normalizedRoots[j]) &&
                !isPathUnderRoot(normalizedRoots[j], normalizedRoots[i])) {
                continue;
            }
            throw DiagnosticError(
                DiagnosticError::Category::Driver,
                "module roots must not overlap: `" +
                    normalizedRoots[i].string() + "` and `" +
                    normalizedRoots[j].string() + "`",
                "Each module root contributes to one flat canonical module "
                "namespace. Remove one of the overlapping roots or move the "
                "sources so no root contains another.");
        }
    }

    std::vector<std::string> roots;
    roots.reserve(normalizedRoots.size());
    for (const auto &root : normalizedRoots) {
        roots.push_back(root.string());
    }
    return roots;
}

std::string
describeConflictingImportCandidates(const std::vector<std::string> &paths) {
    std::string help =
        "This canonical module path matches multiple files: ";
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i != 0) {
            help += "; ";
        }
        help += "`" + paths[i] + "`";
    }
    help += ". Remove one of the conflicting module roots or rename one of "
            "the modules so their canonical paths are unique.";
    return help;
}

std::string
canonicalWorkspaceModulePath(const std::string &path,
                             const std::vector<std::string> &moduleRoots) {
    namespace fs = std::filesystem;
    auto normalized = normalizeModuleFsPath(fs::path(path));
    std::vector<fs::path> matches;
    for (const auto &moduleRoot : moduleRoots) {
        auto includeRoot = normalizeModuleFsPath(fs::path(moduleRoot));
        if (isPathUnderRoot(normalized, includeRoot)) {
            matches.push_back(includeRoot);
        }
    }

    if (matches.empty()) {
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "module `" + normalized.string() +
                "` is outside all configured module roots",
            "The root source directory and explicit `-I` paths define the "
            "canonical module namespace. Known roots: " +
                describeModuleRoots(moduleRoots) + ".");
    }

    if (matches.size() > 1) {
        std::vector<std::string> matchingRoots;
        matchingRoots.reserve(matches.size());
        for (const auto &match : matches) {
            matchingRoots.push_back(match.string());
        }
        throw DiagnosticError(
            DiagnosticError::Category::Driver,
            "module `" + normalized.string() +
                "` belongs to multiple module roots",
            "Overlapping roots make canonical module paths ambiguous. "
            "Matching roots: " +
                describeModuleRoots(matchingRoots) +
                ".");
    }

    auto relative = normalized.lexically_relative(matches.front());
    relative.replace_extension();
    auto modulePath = relative.string();
    if (!modulePath.empty() && modulePath != ".") {
        return modulePath;
    }
    return defaultWorkspaceModulePath(path);
}

std::string
resolveWorkspaceImportPath(const AstImport &importNode,
                           const std::vector<std::string> &moduleRoots) {
    namespace fs = std::filesystem;
    fs::path importPath(importNode.path);
    if (importPath.has_extension()) {
        throw DiagnosticError(DiagnosticError::Category::Syntax, importNode.loc,
                              "import paths should omit the file suffix",
                              "Write imports like `import path/to/file`, not "
                              "`import path/to/file.lo`.");
    }
    if (!importPath.is_relative()) {
        throw DiagnosticError(
            DiagnosticError::Category::Syntax, importNode.loc,
            "import paths must use canonical module paths, not absolute "
            "filesystem paths",
            "Write imports like `import math/ops`, not an absolute file path.");
    }
    importPath += ".lo";

    std::vector<std::string> matches;
    std::vector<fs::path> candidates;
    candidates.reserve(moduleRoots.size());
    for (const auto &moduleRoot : moduleRoots) {
        candidates.push_back(
            normalizeModuleFsPath(fs::path(moduleRoot) / importPath));
    }

    for (const auto &candidate : candidates) {
        std::error_code includeError;
        if (fs::exists(candidate, includeError)) {
            matches.push_back(candidate.string());
            continue;
        }
        if (includeError) {
            auto searchedPath = candidate.string();
            auto includeRoot =
                candidate.parent_path().lexically_normal().string();
            throw DiagnosticError(
                DiagnosticError::Category::Driver, importNode.loc,
                "I couldn't inspect include path `" + includeRoot +
                    "` while resolving import `" + importNode.path + "`.",
                "Check that the include directory exists and that you have "
                "search permission for `" +
                    searchedPath + "`.");
        }
    }

    if (matches.size() > 1) {
        throw DiagnosticError(
            DiagnosticError::Category::Semantic, importNode.loc,
            "found conflicting modules for import `" + importNode.path + "`",
            describeConflictingImportCandidates(matches));
    }

    if (!matches.empty()) {
        return matches.front();
    }

    throw DiagnosticError(
        DiagnosticError::Category::Semantic, importNode.loc,
        "cannot resolve import `" + importNode.path + "`",
        "Import paths must be canonical module paths relative to the root "
        "source directory or an explicit `-I` root. Known module roots: " +
            describeModuleRoots(moduleRoots) + ".");
}

bool
isValidWorkspaceModuleName(const string &name) {
    auto view = name.view();
    if (view.empty()) {
        return false;
    }
    auto isHead = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               ch == '_';
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
    if (node->is<AstImport>() || node->is<AstStructDecl>() ||
        node->is<AstTraitDecl>() || node->is<AstTraitImplDecl>() ||
        node->is<AstFuncDecl>() || node->is<AstGlobalDecl>()) {
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
        includePaths_.push_back(std::filesystem::path(std::move(includePath))
                                    .lexically_normal()
                                    .string());
    }
}

CompilationUnit &
WorkspaceLoader::loadRootUnit(const std::string &path) const {
    auto &unit = workspace_.loadRootUnit(path);
    auto moduleRoots = moduleRootsFor(toStdString(unit.path()));
    unit.setModulePath(
        canonicalWorkspaceModulePath(toStdString(unit.path()), moduleRoots));
    return unit;
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
    auto searchRoots = this->moduleRoots();
    workspace_.moduleGraph().resetDependencies(unit.path());
    unit.clearImportedModules();
    auto *body = requireWorkspaceTopLevelBody(unit);
    for (auto *stmt : body->getBody()) {
        auto *importNode = dynamic_cast<AstImport *>(stmt);
        if (!importNode) {
            continue;
        }
        auto importPath = resolveWorkspaceImportPath(*importNode, searchRoots);
        auto &dependencyUnit = workspace_.loadUnit(importPath);
        dependencyUnit.setModulePath(
            canonicalWorkspaceModulePath(importPath, searchRoots));
        if (!isValidWorkspaceModuleName(dependencyUnit.moduleName())) {
            throw DiagnosticError(
                DiagnosticError::Category::Semantic, importNode->loc,
                "imported module `" + toStdString(dependencyUnit.path()) +
                    "` cannot be referenced as `file.xxx` because `" +
                    toStdString(dependencyUnit.moduleName()) +
                    "` is not a valid identifier",
                "Rename the file so its base name matches identifier syntax.");
        }
        workspace_.moduleGraph().addDependency(unit.path(),
                                               dependencyUnit.path());
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

std::vector<std::string>
WorkspaceLoader::moduleRootsFor(const std::string &rootPath) const {
    return buildWorkspaceModuleRoots(rootPath, includePaths_);
}

std::vector<std::string>
WorkspaceLoader::moduleRoots() const {
    auto *root = workspace_.moduleGraph().root();
    if (root == nullptr) {
        return {};
    }
    return moduleRootsFor(toStdString(root->path()));
}

void
WorkspaceLoader::validateImportedUnit(const CompilationUnit &unit) const {
    (void)unit;
}

}  // namespace lona
