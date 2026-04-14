#include "session.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/pass/compile_pipeline.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/initializer.hh"
#include "lona/sema/hir.hh"
#include "lona/visitor.hh"
#include "lona/scan/driver.hh"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <ios>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lona::tooling {

namespace {

enum class SymbolKind {
    Import,
    Struct,
    Trait,
    Impl,
    Function,
    Method,
    Field,
    Global,
};

struct LocalSymbolRecord {
    std::string kind;
    std::string name;
    std::string detail;
    std::string type;
    SourceLocation loc;
    int scopeDepth = 0;
};

struct FunctionContext {
    const AstFuncDecl *decl = nullptr;
    std::string kind;
    std::string qualifiedName;
    std::string selfDetail;
    bool hasImplicitSelf = false;
    SourceLocation loc;

    explicit operator bool() const { return decl != nullptr; }
};

struct FieldQueryRecord {
    std::string name;
    std::string qualifiedName;
    std::string ownerQualifiedName;
    const TypeNode *typeNode = nullptr;
    SourceLocation loc;
};

struct NamedTypeRecord {
    std::string name;
    std::string qualifiedName;
    const AstStructDecl *decl = nullptr;
    SourceLocation loc;
};

struct FieldLookupResult {
    const FieldQueryRecord *field = nullptr;
    std::vector<const FieldQueryRecord *> candidates;
};

std::string
trimCopy(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(start, end - start));
}

std::string
lowerCopy(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (char ch : text) {
        lowered.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

bool
containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return lowerCopy(haystack).find(lowerCopy(needle)) != std::string::npos;
}

const char *
symbolKindKeyword(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Import:
            return "import";
        case SymbolKind::Struct:
            return "struct";
        case SymbolKind::Trait:
            return "trait";
        case SymbolKind::Impl:
            return "impl";
        case SymbolKind::Function:
            return "func";
        case SymbolKind::Method:
            return "method";
        case SymbolKind::Field:
            return "field";
        case SymbolKind::Global:
            return "global";
    }
    return "symbol";
}

const char *
diagnosticCategoryKeyword(DiagnosticError::Category category) {
    switch (category) {
        case DiagnosticError::Category::Lexical:
            return "lexical";
        case DiagnosticError::Category::Syntax:
            return "syntax";
        case DiagnosticError::Category::Semantic:
            return "semantic";
        case DiagnosticError::Category::Driver:
            return "driver";
        case DiagnosticError::Category::Internal:
            return "internal";
    }
    return "unknown";
}

bool
isTypeLike(SymbolKind kind) {
    return kind == SymbolKind::Struct || kind == SymbolKind::Trait;
}

bool
matchesKindFilter(SymbolKind kind, std::string_view filter) {
    auto normalized = lowerCopy(trimCopy(filter));
    if (normalized.empty() || normalized == "all") {
        return true;
    }
    if (normalized == "type" || normalized == "types") {
        return isTypeLike(kind);
    }
    if (normalized == "struct" || normalized == "structs") {
        return kind == SymbolKind::Struct;
    }
    if (normalized == "trait" || normalized == "traits") {
        return kind == SymbolKind::Trait;
    }
    if (normalized == "impl" || normalized == "impls") {
        return kind == SymbolKind::Impl;
    }
    if (normalized == "func" || normalized == "function" ||
        normalized == "functions") {
        return kind == SymbolKind::Function;
    }
    if (normalized == "method" || normalized == "methods") {
        return kind == SymbolKind::Method;
    }
    if (normalized == "field" || normalized == "fields") {
        return kind == SymbolKind::Field;
    }
    if (normalized == "global" || normalized == "globals") {
        return kind == SymbolKind::Global;
    }
    if (normalized == "import" || normalized == "imports") {
        return kind == SymbolKind::Import;
    }
    return false;
}

SourceLocation
makeSourceLocation(const location &loc, const std::string &fallbackPath) {
    SourceLocation value;
    if (loc.begin.filename != nullptr) {
        value.path = *loc.begin.filename;
    } else {
        value.path = fallbackPath;
    }
    value.line = loc.begin.line;
    value.column = loc.begin.column;
    return value;
}

std::string
locationLabel(const SourceLocation &loc) {
    std::ostringstream out;
    if (!loc.path.empty()) {
        out << loc.path;
        if (loc.line > 0) {
            out << ':';
        }
    }
    if (loc.line > 0) {
        out << loc.line;
        if (loc.column > 0) {
            out << ':' << loc.column;
        }
    }
    return out.str();
}

Json
sourceLocationJson(const SourceLocation &loc) {
    Json root = Json::object();
    root["path"] = loc.path;
    root["line"] = loc.line;
    root["column"] = loc.column;
    return root;
}

Json
symbolJson(const SymbolRecord &symbol) {
    Json root = Json::object();
    root["kind"] = symbol.kind;
    root["name"] = symbol.name;
    root["qualifiedName"] = symbol.qualifiedName;
    root["detail"] = symbol.detail;
    root["location"] = sourceLocationJson(symbol.loc);
    return root;
}

Json
localSymbolJson(const LocalSymbolRecord &symbol) {
    Json root = Json::object();
    root["kind"] = symbol.kind;
    root["name"] = symbol.name;
    root["detail"] = symbol.detail;
    root["type"] = symbol.type;
    root["scopeDepth"] = symbol.scopeDepth;
    root["location"] = sourceLocationJson(symbol.loc);
    return root;
}

std::string
describeGenericParams(const std::vector<AstGenericParam *> *typeParams) {
    if (!typeParams || typeParams->empty()) {
        return {};
    }

    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < typeParams->size(); ++i) {
        auto *param = typeParams->at(i);
        if (!param) {
            continue;
        }
        if (i != 0) {
            out << ", ";
        }
        out << param->name.text.tochara();
        if (param->hasBoundTrait()) {
            out << ' ' << describeDotLikeSyntax(param->boundTrait, "<trait>");
        }
    }
    out << ']';
    return out.str();
}

std::string
describeVarDecl(const AstVarDecl *decl) {
    if (!decl) {
        return {};
    }

    std::ostringstream out;
    if (decl->bindingKind == BindingKind::Ref) {
        out << "ref ";
    }
    out << decl->field.tochara();
    if (decl->typeNode) {
        out << ": " << describeTypeNode(decl->typeNode, "void");
    }
    return out.str();
}

std::string
describeFunctionSignature(const AstFuncDecl *decl) {
    if (!decl) {
        return {};
    }

    std::ostringstream out;
    auto genericParams = describeGenericParams(decl->typeParams);
    if (!genericParams.empty()) {
        out << genericParams;
    }
    out << '(';
    if (decl->args) {
        bool first = true;
        for (auto *arg : *decl->args) {
            auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
            if (!varDecl) {
                continue;
            }
            if (!first) {
                out << ", ";
            }
            first = false;
            out << describeVarDecl(varDecl);
        }
    }
    out << ')';
    if (decl->retType) {
        out << " -> " << describeTypeNode(decl->retType, "void");
    }
    return out.str();
}

std::string
describeTraitImplHeader(const AstTraitImplDecl *decl) {
    if (!decl) {
        return {};
    }
    std::ostringstream out;
    auto genericParams = describeGenericParams(decl->typeParams);
    if (!genericParams.empty()) {
        out << genericParams << ' ';
    }
    out << describeDotLikeSyntax(decl->trait, "<trait>") << " for "
        << describeTypeNode(decl->selfType, "void");
    return out.str();
}

std::string
describeStructHeader(const AstStructDecl *decl) {
    if (!decl) {
        return {};
    }
    auto name = toStdString(decl->name);
    auto genericParams = describeGenericParams(decl->typeParams);
    return genericParams.empty() ? name : name + genericParams;
}

std::string
describeBindingDetail(BindingKind bindingKind) {
    return bindingKind == BindingKind::Ref ? "ref" : std::string();
}

std::string
describeDeclaredType(TypeNode *typeNode) {
    return typeNode ? describeTypeNode(typeNode, "void") : std::string();
}

std::string
describeVarDefDetail(const AstVarDef *decl) {
    if (!decl) {
        return {};
    }
    std::ostringstream out;
    out << varStorageKindKeyword(decl->getStorageKind());
    auto bindingDetail = describeBindingDetail(decl->getBindingKind());
    if (!bindingDetail.empty()) {
        out << ' ' << bindingDetail;
    }
    return out.str();
}

int
locationBeginLine(const location &loc) {
    if (loc.begin.line > 0) {
        return loc.begin.line;
    }
    return loc.end.line > 0 ? loc.end.line : 0;
}

int
locationEndLine(const location &loc) {
    if (loc.end.line > 0) {
        return loc.end.line;
    }
    return loc.begin.line > 0 ? loc.begin.line : 0;
}

bool
locationContainsLine(const location &loc, int line) {
    auto begin = locationBeginLine(loc);
    auto end = locationEndLine(loc);
    if (begin <= 0 || end <= 0) {
        return false;
    }
    if (end < begin) {
        std::swap(begin, end);
    }
    return line >= begin && line <= end;
}

bool
nodeContainsLine(const AstNode *node, int line) {
    return node && locationContainsLine(node->loc, line);
}

bool
subtreeContainsLine(const AstNode *node, int line) {
    if (!node) {
        return false;
    }
    if (nodeContainsLine(node, line)) {
        return true;
    }

    if (auto *program = dynamic_cast<const AstProgram *>(node)) {
        return subtreeContainsLine(program->body, line);
    }
    if (auto *list = dynamic_cast<const AstStatList *>(node)) {
        for (auto *stmt : list->body) {
            if (subtreeContainsLine(stmt, line)) {
                return true;
            }
        }
        return false;
    }
    if (auto *funcDecl = dynamic_cast<const AstFuncDecl *>(node)) {
        if (funcDecl->args) {
            for (auto *arg : *funcDecl->args) {
                if (subtreeContainsLine(arg, line)) {
                    return true;
                }
            }
        }
        return subtreeContainsLine(funcDecl->body, line);
    }
    if (auto *structDecl = dynamic_cast<const AstStructDecl *>(node)) {
        return subtreeContainsLine(structDecl->body, line);
    }
    if (auto *traitDecl = dynamic_cast<const AstTraitDecl *>(node)) {
        return subtreeContainsLine(traitDecl->body, line);
    }
    if (auto *traitImplDecl = dynamic_cast<const AstTraitImplDecl *>(node)) {
        return subtreeContainsLine(traitImplDecl->body, line);
    }
    if (auto *ifNode = dynamic_cast<const AstIf *>(node)) {
        return subtreeContainsLine(ifNode->condition, line) ||
               subtreeContainsLine(ifNode->then, line) ||
               subtreeContainsLine(ifNode->els, line);
    }
    if (auto *forNode = dynamic_cast<const AstFor *>(node)) {
        return subtreeContainsLine(forNode->expr, line) ||
               subtreeContainsLine(forNode->body, line) ||
               subtreeContainsLine(forNode->els, line);
    }
    if (auto *assign = dynamic_cast<const AstAssign *>(node)) {
        return subtreeContainsLine(assign->left, line) ||
               subtreeContainsLine(assign->right, line);
    }
    if (auto *binOp = dynamic_cast<const AstBinOper *>(node)) {
        return subtreeContainsLine(binOp->left, line) ||
               subtreeContainsLine(binOp->right, line);
    }
    if (auto *unary = dynamic_cast<const AstUnaryOper *>(node)) {
        return subtreeContainsLine(unary->expr, line);
    }
    if (auto *refExpr = dynamic_cast<const AstRefExpr *>(node)) {
        return subtreeContainsLine(refExpr->expr, line);
    }
    if (auto *ret = dynamic_cast<const AstRet *>(node)) {
        return subtreeContainsLine(ret->expr, line);
    }
    if (auto *fieldCall = dynamic_cast<const AstFieldCall *>(node)) {
        if (subtreeContainsLine(fieldCall->value, line)) {
            return true;
        }
        if (fieldCall->args) {
            for (auto *arg : *fieldCall->args) {
                if (subtreeContainsLine(arg, line)) {
                    return true;
                }
            }
        }
        return false;
    }
    if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
        return subtreeContainsLine(dotLike->parent, line);
    }
    if (auto *funcRef = dynamic_cast<const AstFuncRef *>(node)) {
        return subtreeContainsLine(funcRef->value, line);
    }
    if (auto *varDef = dynamic_cast<const AstVarDef *>(node)) {
        return subtreeContainsLine(varDef->getInitVal(), line);
    }
    if (auto *globalDecl = dynamic_cast<const AstGlobalDecl *>(node)) {
        return subtreeContainsLine(globalDecl->getInitVal(), line);
    }
    if (auto *varDecl = dynamic_cast<const AstVarDecl *>(node)) {
        return subtreeContainsLine(varDecl->right, line);
    }
    if (auto *tuple = dynamic_cast<const AstTupleLiteral *>(node)) {
        if (!tuple->items) {
            return false;
        }
        for (auto *item : *tuple->items) {
            if (subtreeContainsLine(item, line)) {
                return true;
            }
        }
        return false;
    }
    if (auto *braceInit = dynamic_cast<const AstBraceInit *>(node)) {
        if (!braceInit->items) {
            return false;
        }
        for (auto *item : *braceInit->items) {
            if (subtreeContainsLine(item, line)) {
                return true;
            }
        }
        return false;
    }
    if (auto *braceItem = dynamic_cast<const AstBraceInitItem *>(node)) {
        return subtreeContainsLine(braceItem->value, line);
    }
    if (auto *namedArg = dynamic_cast<const AstNamedCallArg *>(node)) {
        return subtreeContainsLine(namedArg->value, line);
    }
    if (auto *castExpr = dynamic_cast<const AstCastExpr *>(node)) {
        return subtreeContainsLine(castExpr->value, line);
    }
    if (auto *sizeofExpr = dynamic_cast<const AstSizeofExpr *>(node)) {
        return subtreeContainsLine(sizeofExpr->value, line);
    }
    if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node)) {
        return subtreeContainsLine(typeApply->value, line);
    }

    return false;
}

bool
nodeStartsAfterLine(const AstNode *node, int line) {
    if (!node) {
        return false;
    }
    auto begin = locationBeginLine(node->loc);
    return begin > 0 && line < begin;
}

void
findFunctionContextAtLine(const AstNode *node, int line,
                          const std::string &methodOwnerLabel,
                          const std::string &selfDetail,
                          FunctionContext &result) {
    if (!node) {
        return;
    }

    if (auto *program = dynamic_cast<const AstProgram *>(node)) {
        findFunctionContextAtLine(program->body, line, methodOwnerLabel,
                                  selfDetail, result);
        return;
    }

    if (auto *list = dynamic_cast<const AstStatList *>(node)) {
        for (auto *stmt : list->body) {
            findFunctionContextAtLine(stmt, line, methodOwnerLabel, selfDetail,
                                      result);
        }
        return;
    }

    if (auto *structDecl = dynamic_cast<const AstStructDecl *>(node)) {
        if (!subtreeContainsLine(structDecl, line)) {
            return;
        }
        auto ownerLabel = describeStructHeader(structDecl);
        findFunctionContextAtLine(structDecl->body, line, ownerLabel,
                                  ownerLabel, result);
        return;
    }

    if (auto *traitDecl = dynamic_cast<const AstTraitDecl *>(node)) {
        if (!subtreeContainsLine(traitDecl, line)) {
            return;
        }
        auto ownerLabel = toStdString(traitDecl->name);
        findFunctionContextAtLine(traitDecl->body, line, ownerLabel,
                                  ownerLabel, result);
        return;
    }

    if (auto *traitImplDecl = dynamic_cast<const AstTraitImplDecl *>(node)) {
        if (!subtreeContainsLine(traitImplDecl, line)) {
            return;
        }
        auto ownerLabel = describeTraitImplHeader(traitImplDecl);
        findFunctionContextAtLine(
            traitImplDecl->body, line, ownerLabel,
            describeTypeNode(traitImplDecl->selfType, "void"), result);
        return;
    }

    if (auto *funcDecl = dynamic_cast<const AstFuncDecl *>(node)) {
        if (!subtreeContainsLine(funcDecl, line)) {
            return;
        }
        result.decl = funcDecl;
        result.kind = methodOwnerLabel.empty() ? "func" : "method";
        result.qualifiedName = methodOwnerLabel.empty()
                                   ? toStdString(funcDecl->name)
                                   : methodOwnerLabel + "." +
                                         toStdString(funcDecl->name);
        result.selfDetail = selfDetail;
        result.hasImplicitSelf = !methodOwnerLabel.empty();
        result.loc = makeSourceLocation(funcDecl->loc, "");
        findFunctionContextAtLine(funcDecl->body, line, "", "", result);
        return;
    }

    if (auto *ifNode = dynamic_cast<const AstIf *>(node)) {
        if (ifNode->then && subtreeContainsLine(ifNode->then, line)) {
            findFunctionContextAtLine(ifNode->then, line, methodOwnerLabel,
                                      selfDetail, result);
        }
        if (ifNode->els && subtreeContainsLine(ifNode->els, line)) {
            findFunctionContextAtLine(ifNode->els, line, methodOwnerLabel,
                                      selfDetail, result);
        }
        return;
    }

    if (auto *forNode = dynamic_cast<const AstFor *>(node)) {
        if (forNode->body && subtreeContainsLine(forNode->body, line)) {
            findFunctionContextAtLine(forNode->body, line, methodOwnerLabel,
                                      selfDetail, result);
        }
        if (forNode->els && subtreeContainsLine(forNode->els, line)) {
            findFunctionContextAtLine(forNode->els, line, methodOwnerLabel,
                                      selfDetail, result);
        }
    }
}

void
collectLocalsInNode(const AstNode *node, int line, int scopeDepth,
                    const std::string &fallbackPath,
                    std::vector<LocalSymbolRecord> &locals);

void
collectLocalsInBlock(const AstStatList *list, int line, int scopeDepth,
                     const std::string &fallbackPath,
                     std::vector<LocalSymbolRecord> &locals) {
    if (!list) {
        return;
    }

    for (auto *stmt : list->body) {
        if (!stmt) {
            continue;
        }
        if (nodeStartsAfterLine(stmt, line)) {
            break;
        }

        if (auto *varDef = dynamic_cast<const AstVarDef *>(stmt)) {
            locals.push_back(LocalSymbolRecord{
                "local",
                toStdString(varDef->getName()),
                describeVarDefDetail(varDef),
                describeDeclaredType(varDef->getTypeNode()),
                makeSourceLocation(varDef->loc, fallbackPath), scopeDepth});
            continue;
        }

        if (auto *block = dynamic_cast<const AstStatList *>(stmt)) {
            if (subtreeContainsLine(block, line)) {
                collectLocalsInNode(block, line, scopeDepth + 1, fallbackPath,
                                    locals);
                return;
            }
            continue;
        }

        if (auto *ifNode = dynamic_cast<const AstIf *>(stmt)) {
            if (ifNode->then && subtreeContainsLine(ifNode->then, line)) {
                collectLocalsInNode(ifNode->then, line, scopeDepth + 1,
                                    fallbackPath, locals);
                return;
            }
            if (ifNode->els && subtreeContainsLine(ifNode->els, line)) {
                collectLocalsInNode(ifNode->els, line, scopeDepth + 1,
                                    fallbackPath, locals);
                return;
            }
            continue;
        }

        if (auto *forNode = dynamic_cast<const AstFor *>(stmt)) {
            if (forNode->body && subtreeContainsLine(forNode->body, line)) {
                collectLocalsInNode(forNode->body, line, scopeDepth + 1,
                                    fallbackPath, locals);
                return;
            }
            if (forNode->els && subtreeContainsLine(forNode->els, line)) {
                collectLocalsInNode(forNode->els, line, scopeDepth + 1,
                                    fallbackPath, locals);
                return;
            }
        }
    }
}

void
collectLocalsInNode(const AstNode *node, int line, int scopeDepth,
                    const std::string &fallbackPath,
                    std::vector<LocalSymbolRecord> &locals) {
    if (!node) {
        return;
    }
    if (auto *list = dynamic_cast<const AstStatList *>(node)) {
        collectLocalsInBlock(list, line, scopeDepth, fallbackPath, locals);
        return;
    }
    if (auto *ifNode = dynamic_cast<const AstIf *>(node)) {
        if (ifNode->then && subtreeContainsLine(ifNode->then, line)) {
            collectLocalsInNode(ifNode->then, line, scopeDepth + 1,
                                fallbackPath, locals);
            return;
        }
        if (ifNode->els && subtreeContainsLine(ifNode->els, line)) {
            collectLocalsInNode(ifNode->els, line, scopeDepth + 1,
                                fallbackPath, locals);
        }
        return;
    }
    if (auto *forNode = dynamic_cast<const AstFor *>(node)) {
        if (forNode->body && subtreeContainsLine(forNode->body, line)) {
            collectLocalsInNode(forNode->body, line, scopeDepth + 1,
                                fallbackPath, locals);
            return;
        }
        if (forNode->els && subtreeContainsLine(forNode->els, line)) {
            collectLocalsInNode(forNode->els, line, scopeDepth + 1,
                                fallbackPath, locals);
        }
    }
}

std::vector<LocalSymbolRecord>
dedupeVisibleLocals(std::vector<LocalSymbolRecord> locals) {
    std::unordered_set<std::string> seen;
    std::vector<LocalSymbolRecord> deduped;
    deduped.reserve(locals.size());

    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
        if (!seen.emplace(it->name).second) {
            continue;
        }
        deduped.push_back(*it);
    }

    std::reverse(deduped.begin(), deduped.end());
    return deduped;
}

struct SemanticLocalRecord {
    std::string name;
    SourceLocation loc;
    TypeClass *typeRef = nullptr;
    std::string type;
};

bool
hirNodeStartsAfterLine(const HIRNode *node, int line) {
    if (!node) {
        return false;
    }
    auto begin = locationBeginLine(node->getLocation());
    return begin > 0 && line < begin;
}

bool
hirNodeContainsLine(const HIRNode *node, int line) {
    return node && locationContainsLine(node->getLocation(), line);
}

void
collectSemanticLocalsInNode(const HIRNode *node, int line,
                            const std::string &fallbackPath,
                            std::vector<SemanticLocalRecord> &locals);

void
collectAllSemanticLocalsInNode(const HIRNode *node,
                               const std::string &fallbackPath,
                               std::vector<SemanticLocalRecord> &locals);

void
collectSemanticLocalsInBlock(const HIRBlock *block, int line,
                             const std::string &fallbackPath,
                             std::vector<SemanticLocalRecord> &locals) {
    if (!block) {
        return;
    }

    for (auto *node : block->getBody()) {
        if (!node) {
            continue;
        }
        if (hirNodeStartsAfterLine(node, line)) {
            break;
        }

        if (auto *varDef = dynamic_cast<const HIRVarDef *>(node)) {
            locals.push_back(SemanticLocalRecord{
                toStdString(varDef->getName()),
                makeSourceLocation(varDef->getLocation(), fallbackPath),
                varDef->getObject() ? varDef->getObject()->getType() : nullptr,
                describeResolvedType(varDef->getObject()
                                         ? varDef->getObject()->getType()
                                         : nullptr)});
            continue;
        }

        if (auto *childBlock = dynamic_cast<const HIRBlock *>(node)) {
            if (hirNodeContainsLine(childBlock, line)) {
                collectSemanticLocalsInNode(childBlock, line, fallbackPath,
                                            locals);
                return;
            }
            continue;
        }

        if (auto *ifNode = dynamic_cast<const HIRIf *>(node)) {
            if (ifNode->getThenBlock() &&
                hirNodeContainsLine(ifNode->getThenBlock(), line)) {
                collectSemanticLocalsInNode(ifNode->getThenBlock(), line,
                                            fallbackPath, locals);
                return;
            }
            if (ifNode->hasElseBlock() &&
                hirNodeContainsLine(ifNode->getElseBlock(), line)) {
                collectSemanticLocalsInNode(ifNode->getElseBlock(), line,
                                            fallbackPath, locals);
                return;
            }
            continue;
        }

        if (auto *forNode = dynamic_cast<const HIRFor *>(node)) {
            if (forNode->getBody() && hirNodeContainsLine(forNode->getBody(), line)) {
                collectSemanticLocalsInNode(forNode->getBody(), line,
                                            fallbackPath, locals);
                return;
            }
            if (forNode->hasElseBlock() &&
                hirNodeContainsLine(forNode->getElseBlock(), line)) {
                collectSemanticLocalsInNode(forNode->getElseBlock(), line,
                                            fallbackPath, locals);
                return;
            }
        }
    }
}

void
collectSemanticLocalsInNode(const HIRNode *node, int line,
                            const std::string &fallbackPath,
                            std::vector<SemanticLocalRecord> &locals) {
    if (!node) {
        return;
    }
    if (auto *block = dynamic_cast<const HIRBlock *>(node)) {
        collectSemanticLocalsInBlock(block, line, fallbackPath, locals);
        return;
    }
    if (auto *ifNode = dynamic_cast<const HIRIf *>(node)) {
        if (ifNode->getThenBlock() &&
            hirNodeContainsLine(ifNode->getThenBlock(), line)) {
            collectSemanticLocalsInNode(ifNode->getThenBlock(), line,
                                        fallbackPath, locals);
            return;
        }
        if (ifNode->hasElseBlock() &&
            hirNodeContainsLine(ifNode->getElseBlock(), line)) {
            collectSemanticLocalsInNode(ifNode->getElseBlock(), line,
                                        fallbackPath, locals);
        }
        return;
    }
    if (auto *forNode = dynamic_cast<const HIRFor *>(node)) {
        if (forNode->getBody() && hirNodeContainsLine(forNode->getBody(), line)) {
            collectSemanticLocalsInNode(forNode->getBody(), line, fallbackPath,
                                        locals);
            return;
        }
        if (forNode->hasElseBlock() &&
            hirNodeContainsLine(forNode->getElseBlock(), line)) {
            collectSemanticLocalsInNode(forNode->getElseBlock(), line,
                                        fallbackPath, locals);
        }
    }
}

void
collectAllSemanticLocalsInNode(const HIRNode *node,
                               const std::string &fallbackPath,
                               std::vector<SemanticLocalRecord> &locals) {
    if (!node) {
        return;
    }
    if (auto *varDef = dynamic_cast<const HIRVarDef *>(node)) {
        locals.push_back(SemanticLocalRecord{
            toStdString(varDef->getName()),
            makeSourceLocation(varDef->getLocation(), fallbackPath),
            varDef->getObject() ? varDef->getObject()->getType() : nullptr,
            describeResolvedType(varDef->getObject()
                                     ? varDef->getObject()->getType()
                                     : nullptr)});
        return;
    }
    if (auto *block = dynamic_cast<const HIRBlock *>(node)) {
        for (auto *child : block->getBody()) {
            collectAllSemanticLocalsInNode(child, fallbackPath, locals);
        }
        return;
    }
    if (auto *ifNode = dynamic_cast<const HIRIf *>(node)) {
        collectAllSemanticLocalsInNode(ifNode->getThenBlock(), fallbackPath,
                                       locals);
        if (ifNode->hasElseBlock()) {
            collectAllSemanticLocalsInNode(ifNode->getElseBlock(),
                                           fallbackPath, locals);
        }
        return;
    }
    if (auto *forNode = dynamic_cast<const HIRFor *>(node)) {
        collectAllSemanticLocalsInNode(forNode->getBody(), fallbackPath, locals);
        if (forNode->hasElseBlock()) {
            collectAllSemanticLocalsInNode(forNode->getElseBlock(),
                                           fallbackPath, locals);
        }
    }
}

std::string
localSymbolKey(std::string_view name, const SourceLocation &loc) {
    std::ostringstream out;
    out << name << '@' << loc.path << ':' << loc.line << ':' << loc.column;
    return out.str();
}

const AnalyzedFunctionRecord *
findAnalyzedFunctionRecord(const std::vector<AnalyzedFunctionRecord> &records,
                           const AstFuncDecl *decl) {
    for (const auto &record : records) {
        if (record.resolved && record.resolved->decl() == decl) {
            return &record;
        }
    }
    return nullptr;
}

const AnalyzedFunctionRecord *
findTopLevelEntryRecord(const std::vector<AnalyzedFunctionRecord> &records) {
    for (const auto &record : records) {
        if (record.resolved && record.resolved->isTopLevelEntry()) {
            return &record;
        }
    }
    return nullptr;
}

void
enrichLocalsWithAnalysis(const std::vector<AnalyzedFunctionRecord> &records,
                         const FunctionContext &context, int line,
                         const std::string &fallbackPath,
                         std::vector<LocalSymbolRecord> &locals) {
    auto *record = findAnalyzedFunctionRecord(records, context.decl);
    if (!record || !record->hir) {
        return;
    }

    if (record->hir->hasSelfBinding()) {
        auto selfType = describeResolvedType(
            record->hir->getSelfBinding().object
                ? record->hir->getSelfBinding().object->getType()
                : nullptr);
        for (auto &local : locals) {
            if (local.kind == "self") {
                local.type = selfType;
                break;
            }
        }
    }

    std::unordered_map<std::string, std::string> paramTypes;
    for (const auto &param : record->hir->getParams()) {
        paramTypes[toStdString(param.name)] =
            describeResolvedType(param.object ? param.object->getType()
                                              : nullptr);
    }
    for (auto &local : locals) {
        if (local.kind == "param") {
            if (auto found = paramTypes.find(local.name);
                found != paramTypes.end()) {
                local.type = found->second;
            }
        }
    }

    std::vector<SemanticLocalRecord> semanticLocals;
    collectSemanticLocalsInNode(record->hir->getBody(), line, fallbackPath,
                                semanticLocals);
    std::unordered_map<std::string, std::string> localTypes;
    for (const auto &semantic : semanticLocals) {
        localTypes[localSymbolKey(semantic.name, semantic.loc)] = semantic.type;
    }
    for (auto &local : locals) {
        if (local.kind != "local") {
            continue;
        }
        if (auto found = localTypes.find(localSymbolKey(local.name, local.loc));
            found != localTypes.end()) {
            local.type = found->second;
        }
    }
}

Json
functionContextJson(const FunctionContext &context) {
    Json root = Json::object();
    root["kind"] = context.kind;
    root["name"] = context.qualifiedName;
    root["hasImplicitSelf"] = context.hasImplicitSelf;
    root["selfDetail"] = context.selfDetail;
    root["location"] = sourceLocationJson(context.loc);
    return root;
}

bool
collectVisibleLocalsForLine(const AstNode *syntaxTree,
                            const std::vector<AnalyzedFunctionRecord> &records,
                            const std::string &fallbackPath, int line,
                            FunctionContext &context,
                            std::vector<LocalSymbolRecord> &locals,
                            const AnalyzedFunctionRecord *&record) {
    if (!syntaxTree || line <= 0) {
        record = nullptr;
        return false;
    }

    findFunctionContextAtLine(syntaxTree, line, "", "", context);
    if (!context) {
        record = nullptr;
        return false;
    }

    if (context.hasImplicitSelf) {
        locals.push_back(LocalSymbolRecord{
            "self", "self", "", context.selfDetail, context.loc, 0});
    }
    if (context.decl->args) {
        for (auto *arg : *context.decl->args) {
            auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
            if (!varDecl) {
                continue;
            }
            locals.push_back(LocalSymbolRecord{
                "param",
                toStdString(varDecl->field),
                describeBindingDetail(varDecl->bindingKind),
                describeDeclaredType(varDecl->typeNode),
                makeSourceLocation(varDecl->loc, fallbackPath), 0});
        }
    }

    collectLocalsInNode(context.decl->body, line, 0, fallbackPath, locals);
    locals = dedupeVisibleLocals(std::move(locals));
    record = findAnalyzedFunctionRecord(records, context.decl);
    enrichLocalsWithAnalysis(records, context, line, fallbackPath, locals);
    return true;
}

std::unordered_map<std::string, TypeClass *>
collectVisibleLocalTypes(const FunctionContext &context,
                         const AnalyzedFunctionRecord *record, int line,
                         const std::string &fallbackPath) {
    std::unordered_map<std::string, TypeClass *> types;
    if (!record || !record->hir) {
        return types;
    }

    if (record->hir->hasSelfBinding()) {
        types[localSymbolKey("self", context.loc)] =
            record->hir->getSelfBinding().object
                ? record->hir->getSelfBinding().object->getType()
                : nullptr;
    }

    for (const auto &param : record->hir->getParams()) {
        types[localSymbolKey(toStdString(param.name),
                             makeSourceLocation(param.loc, fallbackPath))] =
            param.object ? param.object->getType() : nullptr;
    }

    std::vector<SemanticLocalRecord> semanticLocals;
    collectSemanticLocalsInNode(record->hir->getBody(), line, fallbackPath,
                                semanticLocals);
    for (const auto &semantic : semanticLocals) {
        types[localSymbolKey(semantic.name, semantic.loc)] = semantic.typeRef;
    }
    return types;
}

void
printLocalSymbolLine(std::ostream &out, const LocalSymbolRecord &symbol) {
    out << std::left << std::setw(8) << symbol.kind << ' ' << symbol.name;
    if (!symbol.detail.empty()) {
        out << ' ' << symbol.detail;
    }
    if (!symbol.type.empty()) {
        out << " : " << symbol.type;
    }
    if (symbol.scopeDepth > 0) {
        out << " [depth " << symbol.scopeDepth << ']';
    }
    auto loc = locationLabel(symbol.loc);
    if (!loc.empty()) {
        out << " @" << loc;
    }
    out << '\n';
}

bool
isBuiltinTypeName(std::string_view name) {
    return name == "u8" || name == "i8" || name == "u16" || name == "i16" ||
           name == "u32" || name == "i32" || name == "u64" || name == "i64" ||
           name == "usize" || name == "int" || name == "uint" ||
           name == "f32" || name == "f64" || name == "bool" ||
           name == "void" || name == "any";
}

std::string
describeFieldType(const TypeNode *typeNode) {
    if (!typeNode) {
        return {};
    }
    return describeTypeNode(const_cast<TypeNode *>(typeNode), "void");
}

const TypeNode *
peelConstType(const TypeNode *typeNode) {
    auto *current = typeNode;
    while (auto *constType = dynamic_cast<const ConstTypeNode *>(current)) {
        current = constType->base;
    }
    return current;
}

const BaseTypeNode *
rootBaseTypeNodeForQuery(const TypeNode *typeNode) {
    auto *current = peelConstType(typeNode);
    if (auto *base = dynamic_cast<const BaseTypeNode *>(current)) {
        return base;
    }
    if (auto *applied = dynamic_cast<const AppliedTypeNode *>(current)) {
        return rootBaseTypeNodeForQuery(applied->base);
    }
    return nullptr;
}

const NamedTypeRecord *
findNamedTypeByQualifiedName(const std::vector<NamedTypeRecord> &namedTypes,
                             std::string_view qualifiedName) {
    auto target = std::string(qualifiedName);
    for (const auto &record : namedTypes) {
        if (record.qualifiedName == target) {
            return &record;
        }
    }
    return nullptr;
}

const NamedTypeRecord *
findUniqueNamedTypeByName(const std::vector<NamedTypeRecord> &namedTypes,
                          std::string_view name) {
    auto target = std::string(name);
    const NamedTypeRecord *match = nullptr;
    for (const auto &record : namedTypes) {
        if (record.name != target) {
            continue;
        }
        if (match != nullptr) {
            return nullptr;
        }
        match = &record;
    }
    return match;
}

const NamedTypeRecord *
resolveNamedTypeRecord(const TypeNode *typeNode,
                       std::string_view ownerQualifiedName,
                       const std::vector<NamedTypeRecord> &namedTypes) {
    auto *base = rootBaseTypeNodeForQuery(typeNode);
    if (!base) {
        return nullptr;
    }

    auto rawName = baseTypeName(base);
    if (rawName.empty()) {
        return nullptr;
    }
    if (isBuiltinTypeName(rawName)) {
        return nullptr;
    }

    if (auto *exact = findNamedTypeByQualifiedName(namedTypes, rawName)) {
        return exact;
    }

    if (rawName.find('.') != std::string::npos) {
        return nullptr;
    }

    auto scope = std::string(ownerQualifiedName);
    while (!scope.empty()) {
        auto candidate = scope + "." + rawName;
        if (auto *match =
                findNamedTypeByQualifiedName(namedTypes, candidate)) {
            return match;
        }

        auto separator = scope.rfind('.');
        if (separator == std::string::npos) {
            break;
        }
        scope.erase(separator);
    }

    return findUniqueNamedTypeByName(namedTypes, rawName);
}

void
collectOwnedFieldQueryData(const std::string &ownerLabel, const AstNode *body,
                           const std::string &fallbackPath,
                           std::vector<FieldQueryRecord> &fields,
                           std::vector<NamedTypeRecord> &namedTypes);

void
collectStructFieldQueryData(const AstStructDecl *decl,
                            const std::string &ownerPrefix,
                            const std::string &fallbackPath,
                            std::vector<FieldQueryRecord> &fields,
                            std::vector<NamedTypeRecord> &namedTypes) {
    if (!decl) {
        return;
    }
    auto qualifiedName = ownerPrefix + toStdString(decl->name);
    namedTypes.push_back(NamedTypeRecord{
        toStdString(decl->name), qualifiedName, decl,
        makeSourceLocation(decl->loc, fallbackPath)});
    collectOwnedFieldQueryData(qualifiedName, decl->body, fallbackPath, fields,
                               namedTypes);
}

void
collectTraitFieldQueryData(const AstTraitDecl *decl,
                           const std::string &ownerPrefix,
                           const std::string &fallbackPath,
                           std::vector<FieldQueryRecord> &fields,
                           std::vector<NamedTypeRecord> &namedTypes) {
    if (!decl) {
        return;
    }
    auto qualifiedName = ownerPrefix + toStdString(decl->name);
    collectOwnedFieldQueryData(qualifiedName, decl->body, fallbackPath, fields,
                               namedTypes);
}

void
collectTraitImplFieldQueryData(const AstTraitImplDecl *decl,
                               const std::string &fallbackPath,
                               std::vector<FieldQueryRecord> &fields,
                               std::vector<NamedTypeRecord> &namedTypes) {
    if (!decl) {
        return;
    }
    collectOwnedFieldQueryData(describeTraitImplHeader(decl), decl->body,
                               fallbackPath, fields, namedTypes);
}

void
collectOwnedFieldQueryData(const std::string &ownerLabel, const AstNode *body,
                           const std::string &fallbackPath,
                           std::vector<FieldQueryRecord> &fields,
                           std::vector<NamedTypeRecord> &namedTypes) {
    auto *list = dynamic_cast<const AstStatList *>(body);
    if (!list) {
        return;
    }
    for (auto *stmt : list->body) {
        if (!stmt) {
            continue;
        }
        if (auto *field = dynamic_cast<const AstVarDecl *>(stmt)) {
            auto name = toStdString(field->field);
            fields.push_back(FieldQueryRecord{
                name, ownerLabel + "." + name, ownerLabel, field->typeNode,
                makeSourceLocation(field->loc, fallbackPath)});
            continue;
        }
        if (auto *nestedStruct = dynamic_cast<const AstStructDecl *>(stmt)) {
            collectStructFieldQueryData(nestedStruct, ownerLabel + ".",
                                        fallbackPath, fields, namedTypes);
            continue;
        }
        if (auto *nestedTrait = dynamic_cast<const AstTraitDecl *>(stmt)) {
            collectTraitFieldQueryData(nestedTrait, ownerLabel + ".",
                                       fallbackPath, fields, namedTypes);
            continue;
        }
        if (auto *nestedTraitImpl =
                dynamic_cast<const AstTraitImplDecl *>(stmt)) {
            collectTraitImplFieldQueryData(nestedTraitImpl, fallbackPath,
                                           fields, namedTypes);
        }
    }
}

void
collectFieldQueryData(const AstNode *node, const std::string &fallbackPath,
                      std::vector<FieldQueryRecord> &fields,
                      std::vector<NamedTypeRecord> &namedTypes) {
    if (!node) {
        return;
    }
    if (auto *program = dynamic_cast<const AstProgram *>(node)) {
        collectFieldQueryData(program->body, fallbackPath, fields, namedTypes);
        return;
    }
    if (auto *list = dynamic_cast<const AstStatList *>(node)) {
        for (auto *stmt : list->body) {
            collectFieldQueryData(stmt, fallbackPath, fields, namedTypes);
        }
        return;
    }
    if (auto *structDecl = dynamic_cast<const AstStructDecl *>(node)) {
        collectStructFieldQueryData(structDecl, "", fallbackPath, fields,
                                    namedTypes);
        return;
    }
    if (auto *traitDecl = dynamic_cast<const AstTraitDecl *>(node)) {
        collectTraitFieldQueryData(traitDecl, "", fallbackPath, fields,
                                   namedTypes);
        return;
    }
    if (auto *traitImplDecl = dynamic_cast<const AstTraitImplDecl *>(node)) {
        collectTraitImplFieldQueryData(traitImplDecl, fallbackPath, fields,
                                       namedTypes);
    }
}

Json
fieldCandidateJson(const FieldQueryRecord &field) {
    Json root = Json::object();
    root["name"] = field.name;
    root["qualifiedName"] = field.qualifiedName;
    root["owner"] = field.ownerQualifiedName;
    root["type"] = describeFieldType(field.typeNode);
    root["location"] = sourceLocationJson(field.loc);
    return root;
}

FieldLookupResult
lookupFieldRecord(const std::vector<FieldQueryRecord> &fields,
                  std::string_view fieldName) {
    FieldLookupResult result;
    auto query = trimCopy(fieldName);
    if (query.empty()) {
        return result;
    }

    for (const auto &field : fields) {
        if (field.qualifiedName == query) {
            result.field = &field;
            result.candidates.push_back(&field);
            return result;
        }
    }

    for (const auto &field : fields) {
        if (field.name == query) {
            result.candidates.push_back(&field);
        }
    }

    if (result.candidates.size() == 1) {
        result.field = result.candidates.front();
    }
    return result;
}

Json
makeTypeInfoJson(const TypeNode *typeNode, std::string_view ownerQualifiedName,
                 const std::vector<NamedTypeRecord> &namedTypes,
                 const std::string &fallbackPath,
                 std::unordered_set<std::string> &activeTypes);

Json
makeFieldTypeMemberJson(const AstVarDecl *field,
                        std::string_view ownerQualifiedName,
                        const std::vector<NamedTypeRecord> &namedTypes,
                        const std::string &fallbackPath,
                        std::unordered_set<std::string> &activeTypes) {
    Json root = Json::object();
    auto name = toStdString(field->field);
    root["kind"] = "field";
    root["name"] = name;
    root["qualifiedName"] = std::string(ownerQualifiedName) + "." + name;
    root["type"] = describeFieldType(field->typeNode);
    root["location"] = sourceLocationJson(
        makeSourceLocation(field->loc, fallbackPath));
    root["typeInfo"] = makeTypeInfoJson(field->typeNode, ownerQualifiedName,
                                        namedTypes, fallbackPath, activeTypes);
    return root;
}

Json
makeTupleMemberJson(std::size_t index, const TypeNode *typeNode,
                    std::string_view ownerQualifiedName,
                    const std::vector<NamedTypeRecord> &namedTypes,
                    const std::string &fallbackPath,
                    std::unordered_set<std::string> &activeTypes) {
    Json root = Json::object();
    auto name = "_" + std::to_string(index + 1);
    root["kind"] = "tuple-item";
    root["name"] = name;
    root["qualifiedName"] = std::string(ownerQualifiedName).empty()
                                ? name
                                : std::string(ownerQualifiedName) + "." + name;
    root["type"] = describeFieldType(typeNode);
    root["location"] = sourceLocationJson(
        makeSourceLocation(typeNode ? typeNode->loc : location(),
                           fallbackPath));
    root["typeInfo"] = makeTypeInfoJson(typeNode, ownerQualifiedName,
                                        namedTypes, fallbackPath, activeTypes);
    return root;
}

Json
makeTypeInfoJson(const TypeNode *typeNode, std::string_view ownerQualifiedName,
                 const std::vector<NamedTypeRecord> &namedTypes,
                 const std::string &fallbackPath,
                 std::unordered_set<std::string> &activeTypes) {
    Json root = Json::object();
    root["spelling"] = describeFieldType(typeNode);
    root["kind"] = "unknown";
    root["hasMembers"] = false;
    root["members"] = Json::array();
    root["recursive"] = false;

    if (!typeNode) {
        return root;
    }

    if (auto *constType = dynamic_cast<const ConstTypeNode *>(typeNode)) {
        auto base = makeTypeInfoJson(constType->base, ownerQualifiedName,
                                     namedTypes, fallbackPath, activeTypes);
        root["kind"] = "const";
        root["hasMembers"] = base["hasMembers"];
        root["members"] = base["members"];
        root["recursive"] = base["recursive"];
        auto qualified = base.find("resolvedQualifiedName");
        if (qualified != base.end()) {
            root["resolvedQualifiedName"] = *qualified;
        }
        return root;
    }

    if (auto *tupleType = dynamic_cast<const TupleTypeNode *>(typeNode)) {
        root["kind"] = "tuple";
        for (std::size_t i = 0; i < tupleType->items.size(); ++i) {
            root["members"].push_back(
                makeTupleMemberJson(i, tupleType->items[i], ownerQualifiedName,
                                    namedTypes, fallbackPath, activeTypes));
        }
        root["hasMembers"] = !tupleType->items.empty();
        return root;
    }

    if (dynamic_cast<const PointerTypeNode *>(typeNode)) {
        root["kind"] = "pointer";
        return root;
    }
    if (dynamic_cast<const IndexablePointerTypeNode *>(typeNode)) {
        root["kind"] = "indexable-pointer";
        return root;
    }
    if (dynamic_cast<const ArrayTypeNode *>(typeNode)) {
        root["kind"] = "array";
        return root;
    }
    if (dynamic_cast<const FuncPtrTypeNode *>(typeNode)) {
        root["kind"] = "func-ptr";
        return root;
    }
    if (dynamic_cast<const DynTypeNode *>(typeNode)) {
        root["kind"] = "dyn";
        return root;
    }
    if (dynamic_cast<const AnyTypeNode *>(typeNode)) {
        root["kind"] = "any";
        return root;
    }

    if (auto *resolved =
            resolveNamedTypeRecord(typeNode, ownerQualifiedName, namedTypes)) {
        root["kind"] = "struct";
        root["resolvedQualifiedName"] = resolved->qualifiedName;
        if (!activeTypes.emplace(resolved->qualifiedName).second) {
            root["recursive"] = true;
            return root;
        }

        auto *body = resolved->decl ? resolved->decl->body : nullptr;
        auto *list = dynamic_cast<const AstStatList *>(body);
        if (list) {
            for (auto *stmt : list->body) {
                auto *field = dynamic_cast<const AstVarDecl *>(stmt);
                if (!field) {
                    continue;
                }
                root["members"].push_back(makeFieldTypeMemberJson(
                    field, resolved->qualifiedName, namedTypes, fallbackPath,
                    activeTypes));
            }
        }
        root["hasMembers"] = !root["members"].empty();
        activeTypes.erase(resolved->qualifiedName);
        return root;
    }

    if (auto *base = rootBaseTypeNodeForQuery(typeNode)) {
        auto rawName = baseTypeName(base);
        root["kind"] = isBuiltinTypeName(rawName) ? "builtin" : "named";
        return root;
    }

    if (dynamic_cast<const AppliedTypeNode *>(typeNode)) {
        root["kind"] = "applied";
        return root;
    }

    return root;
}

Json
fieldInfoItemJson(const FieldQueryRecord &field,
                  const std::vector<NamedTypeRecord> &namedTypes,
                  const std::string &fallbackPath) {
    Json root = Json::object();
    root["name"] = field.name;
    root["qualifiedName"] = field.qualifiedName;
    root["owner"] = field.ownerQualifiedName;
    root["type"] = describeFieldType(field.typeNode);
    root["location"] = sourceLocationJson(field.loc);

    std::unordered_set<std::string> activeTypes;
    root["typeInfo"] = makeTypeInfoJson(field.typeNode, field.ownerQualifiedName,
                                        namedTypes, fallbackPath, activeTypes);
    return root;
}

void
printFieldMembers(std::ostream &out, const Json &typeInfo, int indent) {
    if (!typeInfo["hasMembers"].get<bool>()) {
        if (typeInfo["recursive"].get<bool>()) {
            out << std::string(static_cast<std::size_t>(indent), ' ')
                << "members: <recursive>\n";
        }
        return;
    }

    out << std::string(static_cast<std::size_t>(indent), ' ') << "members:\n";
    for (const auto &member : typeInfo["members"]) {
        out << std::string(static_cast<std::size_t>(indent + 2), ' ')
            << member["name"].get<std::string>() << ": "
            << member["type"].get<std::string>();
        SourceLocation loc{
            member["location"]["path"].get<std::string>(),
            member["location"]["line"].get<int>(),
            member["location"]["column"].get<int>()};
        auto locLabel = locationLabel(loc);
        if (!locLabel.empty()) {
            out << " @" << locLabel;
        }
        out << '\n';
        printFieldMembers(out, member["typeInfo"], indent + 4);
    }
}

struct PrintedFieldCandidate {
    std::string ownerName;
    const ModuleInterface::TypeDecl *ownerDecl = nullptr;
    std::string fieldName;
    TypeClass *fieldType = nullptr;
    AccessKind access = AccessKind::GetOnly;
    bool embedded = false;
};

struct OrderedResolvedMember {
    std::string name;
    TypeClass *type = nullptr;
    int index = -1;
    AccessKind access = AccessKind::GetOnly;
    bool embedded = false;
};

std::string
describeInterfaceGenericParams(
    const std::vector<ModuleInterface::GenericParamDecl> &typeParams) {
    if (typeParams.empty()) {
        return {};
    }

    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < typeParams.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << toStdString(typeParams[i].localName);
        if (!typeParams[i].boundTraitName.empty()) {
            out << ' ' << toStdString(typeParams[i].boundTraitName);
        }
    }
    out << ']';
    return out.str();
}

Json
genericParamsJson(
    const std::vector<ModuleInterface::GenericParamDecl> &typeParams) {
    Json root = Json::array();
    for (const auto &typeParam : typeParams) {
        Json item = Json::object();
        item["name"] = toStdString(typeParam.localName);
        item["boundTrait"] = typeParam.boundTraitName.empty()
                                 ? Json(nullptr)
                                 : Json(toStdString(typeParam.boundTraitName));
        root.push_back(std::move(item));
    }
    return root;
}

std::string
describeResolvedFuncSignature(FuncType *funcType,
                              const std::vector<string> *paramNames = nullptr,
                              std::size_t argOffset = 0) {
    if (!funcType) {
        return {};
    }

    std::ostringstream out;
    out << '(';
    const auto &argTypes = funcType->getArgTypes();
    bool first = true;
    for (std::size_t i = argOffset; i < argTypes.size(); ++i) {
        if (!first) {
            out << ", ";
        }
        first = false;
        const auto paramIndex = i - argOffset;
        if (paramNames && paramIndex < paramNames->size() &&
            !paramNames->at(paramIndex).empty()) {
            out << toStdString(paramNames->at(paramIndex)) << ": ";
        }
        if (funcType->getArgBindingKind(i) == BindingKind::Ref) {
            out << "ref ";
        }
        out << describeResolvedType(argTypes[i]);
    }
    out << ')';
    if (funcType->getRetType()) {
        out << " -> " << describeResolvedType(funcType->getRetType());
    }
    return out.str();
}

std::string
describeInterfaceSignature(
    const std::vector<string> &paramNames,
    const std::vector<BindingKind> &paramBindingKinds,
    const std::vector<string> &paramTypeSpellings,
    const string &returnTypeSpelling,
    const std::vector<ModuleInterface::GenericParamDecl> &typeParams = {}) {
    std::ostringstream out;
    auto genericParams = describeInterfaceGenericParams(typeParams);
    if (!genericParams.empty()) {
        out << genericParams;
    }
    out << '(';
    const auto paramCount = std::max(paramNames.size(), paramTypeSpellings.size());
    for (std::size_t i = 0; i < paramCount; ++i) {
        if (i != 0) {
            out << ", ";
        }
        if (i < paramNames.size() && !paramNames[i].empty()) {
            out << toStdString(paramNames[i]) << ": ";
        }
        if (i < paramBindingKinds.size() &&
            paramBindingKinds[i] == BindingKind::Ref) {
            out << "ref ";
        }
        if (i < paramTypeSpellings.size()) {
            out << toStdString(paramTypeSpellings[i]);
        } else {
            out << "<unknown type>";
        }
    }
    out << ')';
    if (!returnTypeSpelling.empty()) {
        out << " -> " << toStdString(returnTypeSpelling);
    }
    return out.str();
}

std::vector<OrderedResolvedMember>
collectOrderedStructMembers(StructType *structType) {
    std::vector<OrderedResolvedMember> members;
    if (!structType) {
        return members;
    }
    members.reserve(structType->getMembers().size());
    for (const auto &entry : structType->getMembers()) {
        members.push_back(OrderedResolvedMember{
            entry.getKey().str(), entry.getValue().first,
            entry.getValue().second,
            structType->getMemberAccess(entry.getKey()),
            structType->isEmbeddedMember(entry.getKey())});
    }
    std::sort(members.begin(), members.end(),
              [](const OrderedResolvedMember &lhs,
                 const OrderedResolvedMember &rhs) {
                  if (lhs.index != rhs.index) {
                      return lhs.index < rhs.index;
                  }
                  return lhs.name < rhs.name;
              });
    return members;
}

Json
makeResolvedTypeInfoJson(TypeClass *type,
                         std::unordered_set<std::string> &activeTypes);

Json
makeResolvedMemberJson(const std::string &ownerName, const std::string &name,
                       TypeClass *type, AccessKind access, bool embedded,
                       std::unordered_set<std::string> &activeTypes) {
    Json root = Json::object();
    root["kind"] = "field";
    root["name"] = name;
    root["qualifiedName"] = ownerName.empty() ? name : ownerName + "." + name;
    root["type"] = describeResolvedType(type);
    root["access"] = accessKindKeyword(access);
    root["embedded"] = embedded;
    root["location"] = sourceLocationJson(SourceLocation{});
    root["typeInfo"] = makeResolvedTypeInfoJson(type, activeTypes);
    return root;
}

Json
makeResolvedTypeInfoJson(TypeClass *type,
                         std::unordered_set<std::string> &activeTypes) {
    Json root = Json::object();
    root["spelling"] = describeResolvedType(type);
    root["kind"] = "unknown";
    root["hasMembers"] = false;
    root["members"] = Json::array();
    root["recursive"] = false;

    if (!type) {
        return root;
    }

    if (auto *qualified = type->as<ConstType>()) {
        auto base = makeResolvedTypeInfoJson(qualified->getBaseType(),
                                             activeTypes);
        root["kind"] = "const";
        root["hasMembers"] = base["hasMembers"];
        root["members"] = base["members"];
        root["recursive"] = base["recursive"];
        return root;
    }
    if (auto *structType = type->as<StructType>()) {
        root["kind"] = "struct";
        auto typeName = toStdString(structType->full_name);
        if (!activeTypes.emplace(typeName).second) {
            root["recursive"] = true;
            return root;
        }
        for (const auto &member : collectOrderedStructMembers(structType)) {
            root["members"].push_back(makeResolvedMemberJson(
                typeName, member.name, member.type, member.access,
                member.embedded, activeTypes));
        }
        root["hasMembers"] = !root["members"].empty();
        activeTypes.erase(typeName);
        return root;
    }
    if (auto *tupleType = type->as<TupleType>()) {
        root["kind"] = "tuple";
        const auto &itemTypes = tupleType->getItemTypes();
        for (std::size_t i = 0; i < itemTypes.size(); ++i) {
            root["members"].push_back(makeResolvedMemberJson(
                toStdString(tupleType->full_name),
                TupleType::buildFieldName(i), itemTypes[i],
                AccessKind::GetOnly, false, activeTypes));
        }
        root["hasMembers"] = !itemTypes.empty();
        return root;
    }
    if (type->as<PointerType>()) {
        root["kind"] = "pointer";
        return root;
    }
    if (type->as<IndexablePointerType>()) {
        root["kind"] = "indexable-pointer";
        return root;
    }
    if (type->as<ArrayType>()) {
        root["kind"] = "array";
        return root;
    }
    if (type->as<FuncType>()) {
        root["kind"] = "func";
        return root;
    }
    if (type->as<DynTraitType>()) {
        root["kind"] = "dyn";
        return root;
    }
    if (type->as<AnyType>()) {
        root["kind"] = "any";
        return root;
    }
    root["kind"] = "builtin";
    return root;
}

Json
makeTypeMethodJson(const std::string &name, const std::string &signature,
                   bool generic, AccessKind receiverAccess) {
    Json root = Json::object();
    root["kind"] = "method";
    root["name"] = name;
    root["signature"] = signature;
    root["generic"] = generic;
    root["receiverAccess"] = accessKindKeyword(receiverAccess);
    return root;
}

Json
collectTypeMethodsJson(const ModuleInterface::TypeDecl &decl) {
    Json methods = Json::array();
    auto *structType = decl.type ? decl.type->as<StructType>() : nullptr;
    if (structType) {
        std::vector<std::pair<std::string, FuncType *>> concreteMethods;
        for (const auto &entry : structType->getMethodTypes()) {
            concreteMethods.emplace_back(entry.getKey().str(), entry.getValue());
        }
        std::sort(concreteMethods.begin(), concreteMethods.end(),
                  [](const auto &lhs, const auto &rhs) {
                      return lhs.first < rhs.first;
                  });
        for (const auto &[name, type] : concreteMethods) {
            std::vector<string> paramNames;
            if (auto *stored = structType->getMethodParamNames(name)) {
                paramNames = *stored;
            }
            const std::size_t argOffset =
                type && type->getArgTypes().size() == paramNames.size() + 1
                    ? 1
                    : 0;
            methods.push_back(makeTypeMethodJson(
                name, describeResolvedFuncSignature(type, &paramNames,
                                                    argOffset),
                false, AccessKind::GetOnly));
        }
    }
    for (const auto &method : decl.methodTemplates) {
        methods.push_back(makeTypeMethodJson(
            toStdString(method.localName),
            describeInterfaceSignature(method.paramNames,
                                       method.paramBindingKinds,
                                       method.paramTypeSpellings,
                                       method.returnTypeSpelling,
                                       method.typeParams),
            true, method.receiverAccess));
    }
    return methods;
}

Json
makeTypePrintItem(const ModuleInterface::TypeDecl &decl) {
    Json root = Json::object();
    root["kind"] = "type";
    root["name"] = toStdString(decl.localName);
    root["qualifiedName"] = toStdString(decl.exportedName);
    root["type"] = decl.type ? describeResolvedType(decl.type)
                             : toStdString(decl.localName);
    root["declKind"] = structDeclKindKeyword(decl.declKind);
    root["genericParams"] = genericParamsJson(decl.typeParams);
    std::unordered_set<std::string> activeTypes;
    root["typeInfo"] = makeResolvedTypeInfoJson(decl.type, activeTypes);
    root["methods"] = collectTypeMethodsJson(decl);
    return root;
}

Json
makeTraitPrintItem(const ModuleInterface::TraitDecl &decl) {
    Json root = Json::object();
    root["kind"] = "trait";
    root["name"] = toStdString(decl.localName);
    root["qualifiedName"] = toStdString(decl.exportedName);
    root["methods"] = Json::array();
    for (const auto &method : decl.methods) {
        Json item = Json::object();
        item["kind"] = "method";
        item["name"] = toStdString(method.localName);
        item["signature"] =
            describeInterfaceSignature(method.paramNames, {},
                                       method.paramTypeSpellings,
                                       method.returnTypeSpelling);
        item["receiverAccess"] = accessKindKeyword(method.receiverAccess);
        root["methods"].push_back(std::move(item));
    }
    return root;
}

Json
makeFunctionPrintItem(const ModuleInterface::FunctionDecl &decl) {
    Json root = Json::object();
    root["kind"] = "func";
    root["name"] = toStdString(decl.localName);
    root["qualifiedName"] = toStdString(decl.symbolName);
    root["signature"] = decl.type
                            ? describeResolvedFuncSignature(decl.type,
                                                            &decl.paramNames)
                            : describeInterfaceSignature(
                                  decl.paramNames, decl.paramBindingKinds,
                                  decl.paramTypeSpellings,
                                  decl.returnTypeSpelling, decl.typeParams);
    root["genericParams"] = genericParamsJson(decl.typeParams);
    return root;
}

Json
makeGlobalPrintItem(const ModuleInterface::GlobalDecl &decl) {
    Json root = Json::object();
    root["kind"] = "global";
    root["name"] = toStdString(decl.localName);
    root["qualifiedName"] = toStdString(decl.symbolName);
    root["type"] = describeResolvedType(decl.type);
    root["extern"] = decl.isExtern;
    return root;
}

Json
makeBindingPrintItem(std::string kind, std::string name,
                     std::string qualifiedName, std::string detail,
                     TypeClass *type, const SourceLocation &loc,
                     std::string contextName) {
    Json root = Json::object();
    root["kind"] = std::move(kind);
    root["name"] = std::move(name);
    root["qualifiedName"] = std::move(qualifiedName);
    root["detail"] = std::move(detail);
    root["type"] = describeResolvedType(type);
    root["location"] = sourceLocationJson(loc);
    root["context"] = std::move(contextName);
    std::unordered_set<std::string> activeTypes;
    root["typeInfo"] = makeResolvedTypeInfoJson(type, activeTypes);
    return root;
}

bool
splitFirstQualifiedComponent(std::string_view query, std::string &head,
                             std::string &tail) {
    auto cleaned = trimCopy(query);
    auto split = cleaned.find('.');
    if (split == std::string::npos || split == 0 || split + 1 >= cleaned.size()) {
        return false;
    }
    head = cleaned.substr(0, split);
    tail = cleaned.substr(split + 1);
    return true;
}

CompilationUnit::TopLevelLookup
lookupQualifiedTopLevelName(const CompilationUnit &unit,
                            std::string_view query) {
    auto direct = unit.lookupTopLevelName(trimCopy(query));
    if (direct.found()) {
        return direct;
    }

    std::string namespaceName;
    std::string memberName;
    if (!splitFirstQualifiedComponent(query, namespaceName, memberName)) {
        return {};
    }

    auto namespaceLookup = unit.lookupTopLevelName(namespaceName);
    if (!namespaceLookup.isModule() || !namespaceLookup.importedModule) {
        return {};
    }
    return unit.lookupTopLevelName(*namespaceLookup.importedModule, memberName);
}

bool
lookupQualifiedPrintItem(const CompilationUnit &unit, std::string_view query,
                         Json &item) {
    auto lookup = lookupQualifiedTopLevelName(unit, query);
    if (lookup.isType() && lookup.typeDecl) {
        item = makeTypePrintItem(*lookup.typeDecl);
        return true;
    }
    if (lookup.isTrait() && lookup.traitDecl) {
        item = makeTraitPrintItem(*lookup.traitDecl);
        return true;
    }
    if (lookup.isFunction() && lookup.functionDecl) {
        item = makeFunctionPrintItem(*lookup.functionDecl);
        return true;
    }
    if (lookup.isGlobal() && lookup.globalDecl) {
        item = makeGlobalPrintItem(*lookup.globalDecl);
        return true;
    }
    return false;
}

bool
lookupVisibleLocalPrintItem(const AstNode *syntaxTree,
                            const std::vector<AnalyzedFunctionRecord> &records,
                            const std::string &fallbackPath, int line,
                            std::string_view query, Json &item) {
    FunctionContext context;
    std::vector<LocalSymbolRecord> locals;
    const AnalyzedFunctionRecord *record = nullptr;
    if (!collectVisibleLocalsForLine(syntaxTree, records, fallbackPath, line,
                                     context, locals, record)) {
        return false;
    }

    const auto cleanedQuery = trimCopy(query);
    const auto localTypes =
        collectVisibleLocalTypes(context, record, line, fallbackPath);
    const auto contextPrefix = context.qualifiedName.empty()
                                   ? std::string()
                                   : context.qualifiedName + ".";

    for (const auto &local : locals) {
        const auto qualifiedName = contextPrefix + local.name;
        if (cleanedQuery != local.name && cleanedQuery != qualifiedName) {
            continue;
        }
        TypeClass *type = nullptr;
        if (auto found = localTypes.find(localSymbolKey(local.name, local.loc));
            found != localTypes.end()) {
            type = found->second;
        }
        item = makeBindingPrintItem(local.kind, local.name, qualifiedName,
                                    local.detail, type, local.loc,
                                    context.qualifiedName);
        return true;
    }
    return false;
}

const AstStatList *
topLevelStatementListForQuery(const AstNode *root) {
    if (auto *program = dynamic_cast<const AstProgram *>(root)) {
        return dynamic_cast<const AstStatList *>(program->body);
    }
    return dynamic_cast<const AstStatList *>(root);
}

bool
lookupTopLevelVarPrintItem(const AstNode *syntaxTree,
                          const std::vector<AnalyzedFunctionRecord> &records,
                          const std::string &fallbackPath,
                          std::string_view query, Json &item) {
    const auto cleanedQuery = trimCopy(query);
    if (cleanedQuery.empty() || cleanedQuery.find('.') != std::string::npos) {
        return false;
    }

    const auto *body = topLevelStatementListForQuery(syntaxTree);
    if (!body) {
        return false;
    }

    const AstVarDef *varDecl = nullptr;
    for (auto *stmt : body->body) {
        auto *candidate = dynamic_cast<const AstVarDef *>(stmt);
        if (!candidate) {
            continue;
        }
        if (toStdString(candidate->getName()) == cleanedQuery) {
            varDecl = candidate;
            break;
        }
    }
    if (!varDecl) {
        return false;
    }

    TypeClass *type = nullptr;
    if (auto *record = findTopLevelEntryRecord(records); record && record->hir) {
        std::vector<SemanticLocalRecord> locals;
        collectAllSemanticLocalsInNode(record->hir->getBody(), fallbackPath,
                                       locals);
        for (const auto &local : locals) {
            if (local.name == cleanedQuery &&
                local.loc.line == varDecl->loc.begin.line) {
                type = local.typeRef;
                break;
            }
        }
    }

    item = makeBindingPrintItem("top-level-var", cleanedQuery, cleanedQuery,
                                describeVarDefDetail(varDecl), type,
                                makeSourceLocation(varDecl->loc, fallbackPath),
                                "<top-level>");
    return true;
}

bool
splitMemberQuery(std::string_view query, std::string &owner,
                 std::string &member) {
    auto cleaned = trimCopy(query);
    auto split = cleaned.rfind('.');
    if (split == std::string::npos || split == 0 || split + 1 >= cleaned.size()) {
        return false;
    }
    owner = cleaned.substr(0, split);
    member = cleaned.substr(split + 1);
    return true;
}

bool
lookupQualifiedFieldCandidate(const CompilationUnit &unit,
                              std::string_view ownerName,
                              std::string_view fieldName,
                              PrintedFieldCandidate &candidate) {
    auto ownerLookup = lookupQualifiedTopLevelName(unit, ownerName);
    if (!ownerLookup.isType() || !ownerLookup.typeDecl ||
        !ownerLookup.typeDecl->type) {
        return false;
    }
    auto *structType = ownerLookup.typeDecl->type->as<StructType>();
    if (!structType) {
        return false;
    }
    auto *member = structType->getMember(fieldName);
    if (!member) {
        return false;
    }
    candidate.ownerName = std::string(ownerName);
    candidate.ownerDecl = ownerLookup.typeDecl;
    candidate.fieldName = std::string(fieldName);
    candidate.fieldType = member->first;
    candidate.access = structType->getMemberAccess(fieldName);
    candidate.embedded = structType->isEmbeddedMember(fieldName);
    return true;
}

std::vector<PrintedFieldCandidate>
findFieldCandidates(const CompilationUnit &unit, std::string_view query) {
    std::vector<PrintedFieldCandidate> matches;
    auto cleaned = trimCopy(query);
    if (cleaned.empty()) {
        return matches;
    }

    std::string ownerName;
    std::string fieldName;
    if (splitMemberQuery(cleaned, ownerName, fieldName)) {
        PrintedFieldCandidate candidate;
        if (lookupQualifiedFieldCandidate(unit, ownerName, fieldName,
                                          candidate)) {
            matches.push_back(std::move(candidate));
        }
        return matches;
    }

    auto *interface = unit.interface();
    if (!interface) {
        return matches;
    }

    for (const auto &entry : interface->types()) {
        auto *structType = entry.second.type ? entry.second.type->as<StructType>()
                                             : nullptr;
        if (!structType) {
            continue;
        }
        auto *member = structType->getMember(cleaned);
        if (!member) {
            continue;
        }
        matches.push_back(PrintedFieldCandidate{
            toStdString(entry.second.localName), &entry.second, cleaned,
            member->first, structType->getMemberAccess(cleaned),
            structType->isEmbeddedMember(cleaned)});
    }
    return matches;
}

Json
makeFieldCandidateSummaryJson(const PrintedFieldCandidate &candidate) {
    Json root = Json::object();
    root["kind"] = "field";
    root["name"] = candidate.fieldName;
    root["qualifiedName"] = candidate.ownerName.empty()
                                ? candidate.fieldName
                                : candidate.ownerName + "." + candidate.fieldName;
    root["type"] = describeResolvedType(candidate.fieldType);
    root["access"] = accessKindKeyword(candidate.access);
    root["embedded"] = candidate.embedded;
    return root;
}

Json
makeFieldPrintItem(const PrintedFieldCandidate &candidate) {
    Json root = Json::object();
    root["kind"] = "field";
    root["name"] = candidate.fieldName;
    root["qualifiedName"] = candidate.ownerName.empty()
                                ? candidate.fieldName
                                : candidate.ownerName + "." + candidate.fieldName;
    root["owner"] = candidate.ownerName;
    root["type"] = describeResolvedType(candidate.fieldType);
    root["access"] = accessKindKeyword(candidate.access);
    root["embedded"] = candidate.embedded;
    root["location"] = sourceLocationJson(SourceLocation{});
    std::unordered_set<std::string> activeTypes;
    root["typeInfo"] = makeResolvedTypeInfoJson(candidate.fieldType, activeTypes);
    return root;
}

void
printMethodItems(std::ostream &out, const Json &methods, int indent = 0) {
    if (methods.empty()) {
        return;
    }
    out << std::string(static_cast<std::size_t>(indent), ' ') << "methods:\n";
    for (const auto &method : methods) {
        out << std::string(static_cast<std::size_t>(indent + 2), ' ')
            << method["name"].get<std::string>() << ' '
            << method["signature"].get<std::string>();
        if (method.contains("generic") && method["generic"].get<bool>()) {
            out << " [generic]";
        }
        out << '\n';
    }
}

class SymbolCollector {
    std::vector<SymbolRecord> &symbols_;
    const std::string &fallbackPath_;

    void append(SymbolKind kind, std::string name, std::string qualifiedName,
                std::string detail, const location &loc) {
        symbols_.push_back(SymbolRecord{
            symbolKindKeyword(kind), std::move(name), std::move(qualifiedName),
            std::move(detail), makeSourceLocation(loc, fallbackPath_)});
    }

    void collectOwnedBody(const std::string &ownerLabel, AstNode *body) {
        auto *list = dynamic_cast<AstStatList *>(body);
        if (!list) {
            return;
        }
        for (auto *stmt : list->getBody()) {
            if (!stmt) {
                continue;
            }
            if (auto *field = dynamic_cast<AstVarDecl *>(stmt)) {
                auto qualified = ownerLabel + "." + toStdString(field->field);
                append(SymbolKind::Field, toStdString(field->field),
                       qualified, describeTypeNode(field->typeNode, "void"),
                       field->loc);
                continue;
            }
            if (auto *method = dynamic_cast<AstFuncDecl *>(stmt)) {
                auto qualified = ownerLabel + "." + toStdString(method->name);
                append(SymbolKind::Method, toStdString(method->name),
                       qualified, describeFunctionSignature(method),
                       method->loc);
                continue;
            }
            if (auto *nestedStruct = dynamic_cast<AstStructDecl *>(stmt)) {
                collectStruct(nestedStruct, ownerLabel + ".");
                continue;
            }
            if (auto *nestedGlobal = dynamic_cast<AstGlobalDecl *>(stmt)) {
                auto qualified =
                    ownerLabel + "." + toStdString(nestedGlobal->getName());
                append(SymbolKind::Global, toStdString(nestedGlobal->getName()),
                       qualified,
                       nestedGlobal->hasTypeNode()
                           ? describeTypeNode(nestedGlobal->getTypeNode(), "void")
                           : std::string(),
                       nestedGlobal->loc);
            }
        }
    }

    void collectStruct(AstStructDecl *decl,
                       const std::string &ownerPrefix = std::string()) {
        auto qualified = ownerPrefix + toStdString(decl->name);
        append(SymbolKind::Struct, toStdString(decl->name), qualified,
               describeGenericParams(decl->typeParams), decl->loc);
        collectOwnedBody(qualified, decl->body);
    }

    void collectTrait(AstTraitDecl *decl,
                      const std::string &ownerPrefix = std::string()) {
        auto qualified = ownerPrefix + toStdString(decl->name);
        append(SymbolKind::Trait, toStdString(decl->name), qualified, "",
               decl->loc);
        collectOwnedBody(qualified, decl->body);
    }

    void collectTraitImpl(AstTraitImplDecl *decl) {
        auto qualified = describeTraitImplHeader(decl);
        append(SymbolKind::Impl, qualified, qualified, "", decl->loc);
        collectOwnedBody(qualified, decl->body);
    }

    void collectTopLevelNode(AstNode *node) {
        if (!node) {
            return;
        }
        if (auto *program = dynamic_cast<AstProgram *>(node)) {
            collectTopLevelNode(program->body);
            return;
        }
        if (auto *list = dynamic_cast<AstStatList *>(node)) {
            for (auto *stmt : list->getBody()) {
                collectTopLevelNode(stmt);
            }
            return;
        }
        if (auto *importNode = dynamic_cast<AstImport *>(node)) {
            append(SymbolKind::Import, importNode->path, importNode->path, "",
                   importNode->loc);
            return;
        }
        if (auto *structDecl = dynamic_cast<AstStructDecl *>(node)) {
            collectStruct(structDecl);
            return;
        }
        if (auto *traitDecl = dynamic_cast<AstTraitDecl *>(node)) {
            collectTrait(traitDecl);
            return;
        }
        if (auto *traitImpl = dynamic_cast<AstTraitImplDecl *>(node)) {
            collectTraitImpl(traitImpl);
            return;
        }
        if (auto *globalDecl = dynamic_cast<AstGlobalDecl *>(node)) {
            append(SymbolKind::Global, toStdString(globalDecl->getName()),
                   toStdString(globalDecl->getName()),
                   globalDecl->hasTypeNode()
                       ? describeTypeNode(globalDecl->getTypeNode(), "void")
                       : std::string(),
                   globalDecl->loc);
            return;
        }
        if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(node)) {
            append(SymbolKind::Function, toStdString(funcDecl->name),
                   toStdString(funcDecl->name),
                   describeFunctionSignature(funcDecl), funcDecl->loc);
        }
    }

public:
    SymbolCollector(std::vector<SymbolRecord> &symbols,
                    const std::string &fallbackPath)
        : symbols_(symbols), fallbackPath_(fallbackPath) {}

    void collect(AstNode *root) { collectTopLevelNode(root); }
};

bool
matchesSymbol(const SymbolRecord &symbol, std::string_view kindFilter,
              std::string_view pattern) {
    auto normalizedKind = lowerCopy(symbol.kind);
    SymbolKind kind = SymbolKind::Function;
    if (normalizedKind == "import") {
        kind = SymbolKind::Import;
    } else if (normalizedKind == "struct") {
        kind = SymbolKind::Struct;
    } else if (normalizedKind == "trait") {
        kind = SymbolKind::Trait;
    } else if (normalizedKind == "impl") {
        kind = SymbolKind::Impl;
    } else if (normalizedKind == "method") {
        kind = SymbolKind::Method;
    } else if (normalizedKind == "field") {
        kind = SymbolKind::Field;
    } else if (normalizedKind == "global") {
        kind = SymbolKind::Global;
    }

    if (!matchesKindFilter(kind, kindFilter)) {
        return false;
    }

    auto cleanedPattern = trimCopy(pattern);
    if (cleanedPattern.empty()) {
        return true;
    }
    return containsCaseInsensitive(symbol.name, cleanedPattern) ||
           containsCaseInsensitive(symbol.qualifiedName, cleanedPattern) ||
           containsCaseInsensitive(symbol.detail, cleanedPattern);
}

void
printSymbolLine(std::ostream &out, const SymbolRecord &symbol) {
    out << std::left << std::setw(8) << symbol.kind << ' '
        << symbol.qualifiedName;
    if (!symbol.detail.empty()) {
        out << ' ' << symbol.detail;
    }
    auto loc = locationLabel(symbol.loc);
    if (!loc.empty()) {
        out << " @" << loc;
    }
    out << '\n';
}

Json
diagnosticJson(const DiagnosticError &diagnostic, const DiagnosticEngine &engine,
               const std::string &fallbackPath) {
    Json root = Json::object();
    root["category"] = diagnosticCategoryKeyword(diagnostic.category());
    root["message"] = diagnostic.what();
    root["hint"] = diagnostic.hint();
    root["rendered"] = engine.render(diagnostic, fallbackPath);
    if (diagnostic.hasLocation()) {
        root["location"] = sourceLocationJson(
            makeSourceLocation(diagnostic.where(), fallbackPath));
    } else {
        root["location"] = nullptr;
    }
    return root;
}

std::vector<string>
collectDependentClosure(const ModuleGraph &moduleGraph, const string &path) {
    std::vector<string> pending = {path};
    std::unordered_set<string> queued = {path};
    for (std::size_t index = 0; index < pending.size(); ++index) {
        for (const auto &dependentPath :
             moduleGraph.dependentsOf(pending[index])) {
            if (queued.emplace(dependentPath).second) {
                pending.push_back(dependentPath);
            }
        }
    }
    return pending;
}

}  // namespace

Session::Session(std::size_t errorLimit)
    : loader_(workspace_), diagnostics_(errorLimit) {}

bool
Session::setRootFile(const std::string &path) {
    currentPath_ = path;
    currentSource_.clear();
    currentSourceIsFile_ = true;
    currentLine_ = 0;
    return rebuildProject();
}

bool
Session::setSourceText(std::string path, std::string sourceText) {
    currentPath_ =
        path.empty() ? std::string("<memory>.lo") : std::move(path);
    currentSource_ = std::move(sourceText);
    currentSourceIsFile_ = false;
    currentLine_ = 0;
    return rebuildProject();
}

bool
Session::reload() {
    if (currentPath_.empty()) {
        return false;
    }
    return rebuildProject();
}

void
Session::resetQueryState() {
    diagnostics_.clear();
    symbols_.clear();
    analysisBuild_.reset();
    resolvedModule_.reset();
    analyzedModule_.reset();
    analyzedFunctions_.clear();
}

bool
Session::rebuildProject() {
    resetQueryState();
    currentUnit_ = nullptr;
    syntaxTree_ = nullptr;
    sourceAvailable_ = false;

    try {
        if (currentSourceIsFile_) {
            loader_.setDiagnosticBag(&diagnostics_);
            auto &unit = loader_.loadRootUnit(currentPath_);
            currentUnit_ = &unit;
            currentPath_ = toStdString(unit.path());
            sourceAvailable_ = true;
            if (currentLine_ > static_cast<int>(unit.source().lineCount())) {
                currentLine_ = static_cast<int>(unit.source().lineCount());
            }
            loader_.loadTransitiveUnits();
            syntaxTree_ = unit.syntaxTree();
        } else {
            const auto &source = workspace_.sourceManager().addSource(
                currentPath_, currentSource_);
            currentPath_ = source.path();
            sourceAvailable_ = true;

            auto &unit = workspace_.moduleGraph().getOrCreate(source);
            currentUnit_ = &unit;
            unit.attachInterface(workspace_.moduleCache().getOrCreate(
                source, unit.moduleKey(), unit.moduleName(),
                unit.modulePath()));
            workspace_.moduleGraph().markRoot(unit.path());
            unit.setSyntaxTree(nullptr);
            if (currentLine_ > static_cast<int>(source.lineCount())) {
                currentLine_ = static_cast<int>(source.lineCount());
            }

            std::istringstream input(unit.source().content());
            Driver driver;
            driver.setDiagnosticBag(&diagnostics_);
            driver.input(&input, unit.source());
            auto *tree = driver.parse();
            if (tree) {
                unit.setSyntaxTree(tree);
                syntaxTree_ = tree;
            }
        }
    } catch (const DiagnosticLimitReached &) {
    } catch (const DiagnosticError &error) {
        (void)diagnostics_.add(error);
    }

    if (currentUnit_) {
        syntaxTree_ = currentUnit_->syntaxTree();
    }
    rebuildSymbolIndex();
    if (currentUnit_) {
        tryCollectSemanticDiagnostics(*currentUnit_);
    }
    return sourceAvailable_;
}

void
Session::invalidateModuleAndDependents(const std::string &path) {
    for (const auto &stalePath :
         collectDependentClosure(workspace_.moduleGraph(), string(path))) {
        auto *unit = workspace_.moduleGraph().find(stalePath);
        if (!unit) {
            continue;
        }
        unit->clearInterface();
    }
}

bool
Session::rebuildProjectFromModule(const std::string &path) {
    resetQueryState();
    currentUnit_ = workspace_.moduleGraph().root();
    syntaxTree_ = currentUnit_ ? currentUnit_->syntaxTree() : nullptr;
    sourceAvailable_ = currentUnit_ != nullptr;
    bool hardFailure = false;

    try {
        loader_.setDiagnosticBag(&diagnostics_);
        const auto &source = workspace_.sourceManager().loadFile(path);
        const auto &normalizedPath = source.path();
        if (normalizedPath == currentPath_) {
            return rebuildProject();
        }

        auto *loadedUnit = workspace_.moduleGraph().find(normalizedPath);
        auto reachablePaths =
            workspace_.moduleGraph().postOrderFrom(string(currentPath_));
        const auto isReachable =
            std::find(reachablePaths.begin(), reachablePaths.end(),
                      string(normalizedPath)) != reachablePaths.end();
        if (!loadedUnit || !isReachable) {
            (void)diagnostics_.add(DiagnosticError(
                DiagnosticError::Category::Driver,
                "module `" + normalizedPath +
                    "` is not part of the current root project",
                "Use `root <path>` to select a different top-level module "
                "before reloading files outside the current project."));
            hardFailure = true;
        } else {
            auto &reloadedUnit = workspace_.loadUnit(normalizedPath);
            invalidateModuleAndDependents(normalizedPath);
            loader_.loadTransitiveUnitsFrom(toStdString(reloadedUnit.path()));
        }
    } catch (const DiagnosticLimitReached &) {
    } catch (const DiagnosticError &error) {
        (void)diagnostics_.add(error);
        hardFailure = error.category() == DiagnosticError::Category::Driver;
    }

    currentUnit_ = workspace_.moduleGraph().root();
    syntaxTree_ = currentUnit_ ? currentUnit_->syntaxTree() : nullptr;
    sourceAvailable_ = currentUnit_ != nullptr;
    if (currentUnit_ &&
        currentLine_ > static_cast<int>(currentUnit_->source().lineCount())) {
        currentLine_ = static_cast<int>(currentUnit_->source().lineCount());
    }
    rebuildSymbolIndex();
    if (currentUnit_) {
        tryCollectSemanticDiagnostics(*currentUnit_);
    }
    return !hardFailure;
}

bool
Session::reloadFile(const std::string &path) {
    if (!currentSourceIsFile_ || currentPath_.empty()) {
        return false;
    }
    return rebuildProjectFromModule(path);
}

bool
Session::gotoLine(int line, std::string *errorMessage) {
    if (currentPath_.empty()) {
        if (errorMessage) {
            *errorMessage = "goto requires an open source";
        }
        return false;
    }
    if (line <= 0) {
        if (errorMessage) {
            *errorMessage = "line number must be positive";
        }
        return false;
    }

    const auto *source = workspace_.sourceManager().find(currentPath_);
    if (!source) {
        if (errorMessage) {
            *errorMessage = "current source is not available";
        }
        return false;
    }
    if (line > static_cast<int>(source->lineCount())) {
        if (errorMessage) {
            *errorMessage = "line " + std::to_string(line) +
                            " is out of range (1-" +
                            std::to_string(source->lineCount()) + ")";
        }
        return false;
    }

    currentLine_ = line;
    return true;
}

void
Session::rebuildSymbolIndex() {
    symbols_.clear();
    if (!syntaxTree_) {
        return;
    }
    SymbolCollector(symbols_, currentPath_).collect(syntaxTree_);
}

void
Session::tryCollectSemanticDiagnostics(CompilationUnit &unit) {
    analysisBuild_.reset();
    resolvedModule_.reset();
    analyzedModule_.reset();
    analyzedFunctions_.clear();
    if (!syntaxTree_ || diagnostics_.hasDiagnostics() || diagnostics_.full()) {
        return;
    }

    try {
        analysisBuild_ =
            std::make_unique<IRBuildState>(unit, defaultTargetTriple());
        auto &build = *analysisBuild_;
        std::unordered_set<string> directDependencyPaths;
        for (const auto &dependencyPath :
             workspace_.moduleGraph().dependenciesOf(unit.path())) {
            directDependencyPaths.insert(dependencyPath);
        }
        for (const auto &dependencyPath :
             workspace_.moduleGraph().postOrderFrom(unit.path())) {
            if (dependencyPath == unit.path()) {
                continue;
            }
            auto *loadedUnit = workspace_.moduleGraph().find(dependencyPath);
            if (loadedUnit == nullptr) {
                throw DiagnosticError(
                    DiagnosticError::Category::Internal,
                    "module graph dependency references a missing unit",
                    "This looks like a tooling/module graph bug.");
            }
            loader_.validateImportedUnit(*loadedUnit);
            collectUnitDeclarations(
                &build.global, *loadedUnit, true,
                directDependencyPaths.contains(dependencyPath));
        }
        collectUnitDeclarations(&build.global, unit, false, false);
        defineUnitGlobals(&build.global, unit);
        resolvedModule_ =
            resolveModule(&build.global, unit.syntaxTree(), &unit, true);
        analyzedModule_ = analyzeModule(&build.global, *resolvedModule_, &unit);
        if (!analyzedModule_) {
            analysisBuild_.reset();
            return;
        }
        auto hirIndex = std::size_t{0};
        for (const auto &resolvedFunction : resolvedModule_->functions()) {
            if (resolvedFunction->isTemplateValidationOnly()) {
                continue;
            }
            if (hirIndex >= analyzedModule_->getFunctions().size()) {
                analyzedFunctions_.clear();
                break;
            }
            analyzedFunctions_.push_back(
                {resolvedFunction.get(),
                 analyzedModule_->getFunctions()[hirIndex]});
            ++hirIndex;
        }
        if (!analyzedFunctions_.empty() &&
            hirIndex != analyzedModule_->getFunctions().size()) {
            analyzedFunctions_.clear();
        }
    } catch (const DiagnosticError &error) {
        analysisBuild_.reset();
        (void)diagnostics_.add(error);
    } catch (const DiagnosticLimitReached &) {
        analysisBuild_.reset();
    }
}

Json
Session::statusJson() const {
    Json root = Json::object();
    root["loaded"] = sourceAvailable_;
    if (currentPath_.empty()) {
        root["path"] = nullptr;
        root["sourceKind"] = "none";
    } else {
        root["path"] = currentPath_;
        root["sourceKind"] = currentSourceIsFile_ ? "file" : "memory";
    }
    root["hasTree"] = hasTree();
    if (currentLine_ > 0) {
        root["cursorLine"] = currentLine_;
        root["cursor"] = cursorJson();
    } else {
        root["cursorLine"] = nullptr;
        root["cursor"] = nullptr;
    }
    root["symbolCount"] = symbols_.size();
    root["diagnosticCount"] = diagnostics_.size();
    root["diagnosticsTruncated"] = diagnostics_.truncated();
    root["errorLimit"] = diagnostics_.maxErrors();
    root["hasResolvedModule"] = resolvedModule_ != nullptr;
    root["hasAnalysis"] = analyzedModule_ != nullptr;
    root["analyzedFunctionCount"] = analyzedFunctions_.size();
    return root;
}

Json
Session::cursorJson() const {
    Json root = Json::object();
    if (currentPath_.empty()) {
        root["path"] = nullptr;
        root["line"] = nullptr;
        root["lineCount"] = 0;
        root["hasLocalScope"] = false;
        root["context"] = nullptr;
        return root;
    }

    root["path"] = currentPath_;
    const auto *source = workspace_.sourceManager().find(currentPath_);
    root["lineCount"] = source ? source->lineCount() : 0;
    if (currentLine_ <= 0) {
        root["line"] = nullptr;
        root["hasLocalScope"] = false;
        root["context"] = nullptr;
        return root;
    }

    root["line"] = currentLine_;
    FunctionContext context;
    if (syntaxTree_) {
        findFunctionContextAtLine(syntaxTree_, currentLine_, "", "", context);
    }
    root["hasLocalScope"] = static_cast<bool>(context);
    if (context) {
        root["context"] = functionContextJson(context);
    } else {
        root["context"] = nullptr;
    }
    return root;
}

Json
Session::astJson() const {
    Json root = Json::object();
    if (currentPath_.empty()) {
        root["path"] = nullptr;
    } else {
        root["path"] = currentPath_;
    }
    root["hasTree"] = hasTree();
    if (syntaxTree_) {
        root["ast"] = Json::object();
        syntaxTree_->toJson(root["ast"]);
    } else {
        root["ast"] = nullptr;
    }
    return root;
}

Json
Session::diagnosticsJson() const {
    Json root = Json::object();
    if (currentPath_.empty()) {
        root["path"] = nullptr;
    } else {
        root["path"] = currentPath_;
    }
    root["count"] = diagnostics_.size();
    root["truncated"] = diagnostics_.truncated();
    root["errorLimit"] = diagnostics_.maxErrors();
    root["items"] = Json::array();
    for (const auto &diagnostic : diagnostics_.diagnostics()) {
        root["items"].push_back(
            diagnosticJson(diagnostic, workspace_.diagnostics(), currentPath_));
    }
    return root;
}

Json
Session::symbolsJson() const {
    Json root = Json::object();
    if (currentPath_.empty()) {
        root["path"] = nullptr;
    } else {
        root["path"] = currentPath_;
    }
    root["count"] = symbols_.size();
    root["items"] = Json::array();
    for (const auto &symbol : symbols_) {
        root["items"].push_back(symbolJson(symbol));
    }
    return root;
}

Json
Session::findResultsJson(std::string_view kindFilter,
                         std::string_view pattern) const {
    Json root = Json::object();
    if (currentPath_.empty()) {
        root["path"] = nullptr;
    } else {
        root["path"] = currentPath_;
    }
    root["kindFilter"] = trimCopy(kindFilter);
    root["pattern"] = trimCopy(pattern);
    root["count"] = 0;
    root["items"] = Json::array();
    for (const auto &symbol : symbols_) {
        if (!matchesSymbol(symbol, kindFilter, pattern)) {
            continue;
        }
        root["items"].push_back(symbolJson(symbol));
    }
    root["count"] = root["items"].size();
    return root;
}

Json
Session::fieldInfoJson(std::string_view fieldName) const {
    Json root = Json::object();
    if (currentPath_.empty()) {
        root["path"] = nullptr;
    } else {
        root["path"] = currentPath_;
    }
    root["query"] = trimCopy(fieldName);
    root["found"] = false;
    root["ambiguous"] = false;
    root["item"] = nullptr;
    root["candidates"] = Json::array();
    root["error"] = nullptr;

    if (!currentUnit_ || !currentUnit_->interface() ||
        !currentUnit_->interface()->collected()) {
        root["error"] = "no resolved interface available";
        return root;
    }

    const auto query = trimCopy(fieldName);
    if (query.empty()) {
        root["error"] = "empty print query";
        return root;
    }

    Json printItem = Json::object();
    if (lookupVisibleLocalPrintItem(syntaxTree_, analyzedFunctions_,
                                    currentPath_, currentLine_, query,
                                    printItem)) {
        root["found"] = true;
        root["item"] = std::move(printItem);
        return root;
    }

    if (lookupTopLevelVarPrintItem(syntaxTree_, analyzedFunctions_,
                                   currentPath_, query, printItem)) {
        root["found"] = true;
        root["item"] = std::move(printItem);
        return root;
    }

    if (lookupQualifiedPrintItem(*currentUnit_, query, printItem)) {
        root["found"] = true;
        root["item"] = std::move(printItem);
        return root;
    }

    auto fieldMatches = findFieldCandidates(*currentUnit_, query);
    if (fieldMatches.size() == 1) {
        root["found"] = true;
        root["item"] = makeFieldPrintItem(fieldMatches.front());
        return root;
    }
    if (fieldMatches.empty()) {
        root["error"] = "unknown symbol: " + query;
        return root;
    }

    root["ambiguous"] = true;
    root["error"] = "ambiguous field: " + query;
    for (const auto &candidate : fieldMatches) {
        root["candidates"].push_back(makeFieldCandidateSummaryJson(candidate));
    }
    return root;
}

Json
Session::infoLocalJson(int line) const {
    Json root = Json::object();
    root["hasAnalysis"] = analyzedModule_ != nullptr;
    if (currentPath_.empty()) {
        root["path"] = nullptr;
        root["line"] = nullptr;
        root["hasLocalScope"] = false;
        root["context"] = nullptr;
        root["count"] = 0;
        root["items"] = Json::array();
        return root;
    }

    const auto effectiveLine = line > 0 ? line : currentLine_;
    root["path"] = currentPath_;
    if (effectiveLine <= 0) {
        root["line"] = nullptr;
        root["hasLocalScope"] = false;
        root["context"] = nullptr;
        root["count"] = 0;
        root["items"] = Json::array();
        return root;
    }

    root["line"] = effectiveLine;
    root["items"] = Json::array();

    FunctionContext context;
    std::vector<LocalSymbolRecord> locals;
    const AnalyzedFunctionRecord *record = nullptr;
    if (!collectVisibleLocalsForLine(syntaxTree_, analyzedFunctions_,
                                     currentPath_, effectiveLine, context,
                                     locals, record)) {
        root["hasLocalScope"] = false;
        root["context"] = nullptr;
        root["count"] = 0;
        return root;
    }
    (void)record;

    root["hasLocalScope"] = true;
    root["context"] = functionContextJson(context);
    for (const auto &local : locals) {
        root["items"].push_back(localSymbolJson(local));
    }
    root["count"] = root["items"].size();
    return root;
}

void
Session::printAst(std::ostream &out) const {
    if (!syntaxTree_) {
        out << "no syntax tree available\n";
        return;
    }
    Json root = Json::object();
    syntaxTree_->toJson(root);
    out << root.dump(2) << '\n';
}

void
Session::printDiagnostics(std::ostream &out) const {
    if (!diagnostics_.hasDiagnostics()) {
        out << "no diagnostics\n";
        return;
    }
    bool first = true;
    for (const auto &diagnostic : diagnostics_.diagnostics()) {
        if (!first) {
            out << '\n';
        }
        first = false;
        out << workspace_.diagnostics().render(diagnostic, currentPath_);
    }
    if (diagnostics_.truncated()) {
        out << "note: stopped after reaching the error limit ("
            << diagnostics_.maxErrors() << ")\n";
    }
}

void
Session::printSymbols(std::ostream &out) const {
    if (symbols_.empty()) {
        out << "no symbols\n";
        return;
    }
    for (const auto &symbol : symbols_) {
        printSymbolLine(out, symbol);
    }
}

void
Session::printFindResults(std::ostream &out, std::string_view kindFilter,
                          std::string_view pattern) const {
    std::size_t matchCount = 0;
    for (const auto &symbol : symbols_) {
        if (!matchesSymbol(symbol, kindFilter, pattern)) {
            continue;
        }
        printSymbolLine(out, symbol);
        ++matchCount;
    }
    if (matchCount == 0) {
        out << "no matching symbols\n";
    }
}

void
Session::printFieldInfo(std::ostream &out, std::string_view fieldName) const {
    auto root = fieldInfoJson(fieldName);
    if (!root["found"].get<bool>()) {
        if (!root["error"].is_null()) {
            out << root["error"].get<std::string>() << '\n';
        }
        if (root["ambiguous"].get<bool>()) {
            out << "candidates:\n";
            for (const auto &candidate : root["candidates"]) {
                out << "  " << candidate["qualifiedName"].get<std::string>()
                    << ": " << candidate["type"].get<std::string>() << '\n';
            }
        }
        return;
    }

    const auto &item = root["item"];
    const auto kind = item["kind"].get<std::string>();
    if (kind == "field") {
        out << "field " << item["qualifiedName"].get<std::string>() << '\n';
        out << "type: " << item["type"].get<std::string>() << '\n';
        printFieldMembers(out, item["typeInfo"], 0);
        return;
    }
    if (kind == "type") {
        out << "type " << item["name"].get<std::string>() << " ["
            << item["declKind"].get<std::string>() << "]\n";
        out << "type: " << item["type"].get<std::string>() << '\n';
        printFieldMembers(out, item["typeInfo"], 0);
        printMethodItems(out, item["methods"]);
        return;
    }
    if (kind == "trait") {
        out << "trait " << item["name"].get<std::string>() << '\n';
        printMethodItems(out, item["methods"]);
        return;
    }
    if (kind == "func") {
        out << "func " << item["name"].get<std::string>() << ' '
            << item["signature"].get<std::string>() << '\n';
        return;
    }
    if (kind == "global") {
        out << "global " << item["name"].get<std::string>() << ' '
            << item["type"].get<std::string>() << '\n';
        return;
    }
    if (kind == "local" || kind == "param" || kind == "self") {
        out << kind << ' ' << item["qualifiedName"].get<std::string>() << '\n';
        if (!item["detail"].get<std::string>().empty()) {
            out << "detail: " << item["detail"].get<std::string>() << '\n';
        }
        out << "type: " << item["type"].get<std::string>() << '\n';
        printFieldMembers(out, item["typeInfo"], 0);
        return;
    }
    if (kind == "top-level-var") {
        out << "top-level-var " << item["qualifiedName"].get<std::string>()
            << '\n';
        if (!item["detail"].get<std::string>().empty()) {
            out << "detail: " << item["detail"].get<std::string>() << '\n';
        }
        out << "type: " << item["type"].get<std::string>() << '\n';
        printFieldMembers(out, item["typeInfo"], 0);
        return;
    }
    out << "unknown item kind: " << kind << '\n';
}

void
Session::printInfoLocal(std::ostream &out, int line) const {
    auto root = infoLocalJson(line);
    if (root["line"].is_null()) {
        out << "no analysis point; use `goto <line>` first\n";
        return;
    }
    if (!root["hasLocalScope"].get<bool>()) {
        out << "no local scope at " << root["path"].get<std::string>() << ':'
            << root["line"].get<int>() << '\n';
        return;
    }

    const auto &context = root["context"];
    out << context["kind"].get<std::string>() << ' '
        << context["name"].get<std::string>() << '\n';
    if (root["items"].empty()) {
        out << "no locals in scope\n";
        return;
    }

    for (const auto &item : root["items"]) {
        printLocalSymbolLine(
            out,
            LocalSymbolRecord{
                item["kind"].get<std::string>(),
                item["name"].get<std::string>(),
                item["detail"].get<std::string>(),
                item["type"].get<std::string>(),
                SourceLocation{
                    item["location"]["path"].get<std::string>(),
                    item["location"]["line"].get<int>(),
                    item["location"]["column"].get<int>()},
                item["scopeDepth"].get<int>()});
    }
}

}  // namespace lona::tooling
