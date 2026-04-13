#include "session.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/pass/compile_pipeline.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/hir.hh"
#include "lona/visitor.hh"
#include "lona/scan/driver.hh"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <ios>
#include <optional>
#include <sstream>
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
describeBindingTypeDetail(BindingKind bindingKind, TypeNode *typeNode) {
    std::ostringstream out;
    if (bindingKind == BindingKind::Ref) {
        out << "ref";
    }
    if (typeNode) {
        if (out.tellp() > 0) {
            out << ' ';
        }
        out << describeTypeNode(typeNode, "void");
    }
    return out.str();
}

std::string
describeVarDefDetail(const AstVarDef *decl) {
    if (!decl) {
        return {};
    }
    std::ostringstream out;
    out << varStorageKindKeyword(decl->getStorageKind());
    auto bindingDetail =
        describeBindingTypeDetail(decl->getBindingKind(), decl->getTypeNode());
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

void
printLocalSymbolLine(std::ostream &out, const LocalSymbolRecord &symbol) {
    out << std::left << std::setw(8) << symbol.kind << ' ' << symbol.name;
    if (!symbol.detail.empty()) {
        out << ' ' << symbol.detail;
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

}  // namespace

Session::Session(std::size_t errorLimit) : diagnostics_(errorLimit) {}

bool
Session::openFile(const std::string &path) {
    currentPath_ = path;
    currentSource_.clear();
    currentSourceIsFile_ = true;
    currentLine_ = 0;
    return rebuild();
}

bool
Session::setSourceText(std::string path, std::string sourceText) {
    currentPath_ =
        path.empty() ? std::string("<memory>.lo") : std::move(path);
    currentSource_ = std::move(sourceText);
    currentSourceIsFile_ = false;
    currentLine_ = 0;
    return rebuild();
}

bool
Session::reload() {
    if (currentPath_.empty()) {
        return false;
    }
    return rebuild();
}

bool
Session::rebuild() {
    diagnostics_.clear();
    symbols_.clear();
    syntaxTree_ = nullptr;
    sourceAvailable_ = false;

    try {
        const SourceBuffer *source = nullptr;
        if (currentSourceIsFile_) {
            source = &workspace_.sourceManager().loadFile(currentPath_);
            currentPath_ = source->path();
        } else {
            source = &workspace_.sourceManager().addSource(currentPath_,
                                                          currentSource_);
            currentPath_ = source->path();
        }
        sourceAvailable_ = true;

        auto &unit = workspace_.moduleGraph().getOrCreate(*source);
        unit.attachInterface(workspace_.moduleCache().getOrCreate(
            *source, unit.moduleKey(), unit.moduleName(), unit.modulePath()));
        workspace_.moduleGraph().markRoot(unit.path());
        unit.setSyntaxTree(nullptr);
        if (currentLine_ > static_cast<int>(source->lineCount())) {
            currentLine_ = static_cast<int>(source->lineCount());
        }

        std::istringstream input(unit.source().content());
        Driver driver;
        driver.setDiagnosticBag(&diagnostics_);
        driver.input(&input, unit.source());
        try {
            auto *tree = driver.parse();
            if (tree) {
                unit.setSyntaxTree(tree);
                syntaxTree_ = tree;
            }
        } catch (const DiagnosticLimitReached &) {
            syntaxTree_ = nullptr;
        } catch (const DiagnosticError &error) {
            (void)diagnostics_.add(error);
            syntaxTree_ = nullptr;
        }

        rebuildSymbolIndex();
        tryCollectSemanticDiagnostics(unit);
        return syntaxTree_ != nullptr;
    } catch (const DiagnosticError &error) {
        (void)diagnostics_.add(error);
        return false;
    }
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
    if (!syntaxTree_ ||
        diagnostics_.hasCategory(DiagnosticError::Category::Syntax) ||
        diagnostics_.full()) {
        return;
    }

    try {
        IRBuildState build(unit, defaultTargetTriple());
        collectUnitDeclarations(&build.global, unit, false, false);
        defineUnitGlobals(&build.global, unit);
        auto resolved =
            resolveModule(&build.global, unit.syntaxTree(), &unit, true);
        (void)analyzeModule(&build.global, *resolved, &unit);
    } catch (const DiagnosticError &error) {
        (void)diagnostics_.add(error);
    } catch (const DiagnosticLimitReached &) {
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
Session::infoLocalJson(int line) const {
    Json root = Json::object();
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
    if (syntaxTree_) {
        findFunctionContextAtLine(syntaxTree_, effectiveLine, "", "", context);
    }

    if (!context) {
        root["hasLocalScope"] = false;
        root["context"] = nullptr;
        root["count"] = 0;
        return root;
    }

    std::vector<LocalSymbolRecord> locals;
    if (context.hasImplicitSelf) {
        locals.push_back(LocalSymbolRecord{
            "self", "self", context.selfDetail, context.loc, 0});
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
                describeBindingTypeDetail(varDecl->bindingKind,
                                          varDecl->typeNode),
                makeSourceLocation(varDecl->loc, currentPath_), 0});
        }
    }

    collectLocalsInNode(context.decl->body, effectiveLine, 0, currentPath_,
                        locals);
    locals = dedupeVisibleLocals(std::move(locals));

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
                SourceLocation{
                    item["location"]["path"].get<std::string>(),
                    item["location"]["line"].get<int>(),
                    item["location"]["column"].get<int>()},
                item["scopeDepth"].get<int>()});
    }
}

}  // namespace lona::tooling
