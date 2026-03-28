#include "tag_apply.hh"
#include "astnode.hh"
#include "lona/err/err.hh"
#include <cstddef>
#include <list>
#include <string>
#include <vector>

namespace lona {

namespace tag_apply_impl {

std::string
tokenText(const AstToken *token) {
    if (!token) {
        return {};
    }
    return std::string(token->text.tochara(), token->text.size());
}

std::string
tagName(const AstTag *tag) {
    return tag ? tokenText(&tag->name) : std::string();
}

std::string
describeTagTarget(const AstNode *target) {
    if (auto *funcDecl = dynamic_cast<const AstFuncDecl *>(target)) {
        return "function `" + toStdString(funcDecl->name) + "`";
    }
    if (auto *structDecl = dynamic_cast<const AstStructDecl *>(target)) {
        return "struct `" + toStdString(structDecl->name) + "`";
    }
    if (auto *varDef = dynamic_cast<const AstVarDef *>(target)) {
        return "variable `" + toStdString(varDef->getName()) + "`";
    }
    return "node";
}

[[noreturn]] void
errorUnknownTag(const AstTag *tag, AstNode *target) {
    throw DiagnosticError(
        DiagnosticError::Category::Semantic,
        tag ? tag->name.loc : location(),
        "unknown tag `" + tagName(tag) + "` on " + describeTagTarget(target),
        "Only `extern` and `repr` are supported right now.");
}

void
requireTagArgCount(const AstTag *tag, std::size_t expected,
                   AstNode *target, const std::string &usage) {
    const std::size_t actual = tag && tag->args ? tag->args->size() : 0;
    if (actual == expected) {
        return;
    }
    throw DiagnosticError(
        DiagnosticError::Category::Semantic,
        tag ? tag->name.loc : location(),
        "invalid arguments for tag `" + tagName(tag) + "` on " +
            describeTagTarget(target) + ": expected " +
            std::to_string(expected) + " argument" +
            (expected == 1 ? "" : "s") + ", got " +
            std::to_string(actual),
        usage);
}

std::string
requireStringTagArg(const AstTag *tag, std::size_t index,
                    AstNode *target, const std::string &usage) {
    if (!tag || !tag->args || index >= tag->args->size()) {
        requireTagArgCount(tag, index + 1, target, usage);
    }
    auto *arg = (*tag->args)[index];
    if (!arg || arg->type != TokenType::ConstStr) {
        throw DiagnosticError(
            DiagnosticError::Category::Semantic,
            arg ? arg->loc : (tag ? tag->name.loc : location()),
            "invalid arguments for tag `" + tagName(tag) + "` on " +
                describeTagTarget(target) +
                ": argument " + std::to_string(index) +
                " must be a string literal",
            usage);
    }
    return tokenText(arg);
}

[[noreturn]] void
errorCannotApplyTag(const AstTag *tag, AstNode *target, const std::string &hint) {
    throw DiagnosticError(
        DiagnosticError::Category::Semantic,
        tag ? tag->name.loc : (target ? target->loc : location()),
        "cannot apply tag `" + tagName(tag) + "` to " +
            describeTagTarget(target),
        hint);
}

void
applyExternTag(AstNode *target, const AstTag *tag) {
    if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(target)) {
        if (funcDecl->isExternC()) {
            throw DiagnosticError(
                DiagnosticError::Category::Semantic,
                tag ? tag->name.loc : funcDecl->loc,
                "duplicate `extern` tag on " + describeTagTarget(funcDecl),
                "Write a single tag like `#[extern \"C\"]`.");
        }
        requireTagArgCount(tag, 1, funcDecl, "Use syntax like `#[extern \"C\"]`.");
        auto abi =
            requireStringTagArg(tag, 0, funcDecl, "Use syntax like `#[extern \"C\"]`.");
        if (abi != "C") {
            throw DiagnosticError(
                DiagnosticError::Category::Semantic,
                tag ? tag->name.loc : funcDecl->loc,
                "unsupported extern ABI `" + abi + "`",
                "Only `#[extern \"C\"]` is supported right now.");
        }
        funcDecl->setAbiKind(AbiKind::C);
        return;
    }

    if (auto *structDecl = dynamic_cast<AstStructDecl *>(target)) {
        errorCannotApplyTag(
            tag, target,
            "Write `struct " + toStdString(structDecl->name) +
                "` for an opaque type, or use `#[repr \"C\"] struct " +
                toStdString(structDecl->name) +
                " { ... }` for a C-compatible layout. The `extern` tag only applies to function declarations.");
    }

    if (dynamic_cast<AstVarDef *>(target)) {
        errorCannotApplyTag(
            tag, target,
            "The `extern` tag only applies to function declarations right now.");
    }

    errorCannotApplyTag(
        tag, target,
        "Tags can only be applied to function declarations, struct declarations, and variable definitions.");
}

void
applyReprTag(AstNode *target, const AstTag *tag) {
    if (auto *structDecl = dynamic_cast<AstStructDecl *>(target)) {
        if (structDecl->declKind != StructDeclKind::Native) {
            throw DiagnosticError(
                DiagnosticError::Category::Semantic,
                tag ? tag->name.loc : structDecl->loc,
                "conflicting declaration tags on " +
                    describeTagTarget(structDecl),
                "Choose exactly one struct declaration kind.");
        }
        requireTagArgCount(tag, 1, structDecl, "Use syntax like `#[repr \"C\"]`.");
        auto repr =
            requireStringTagArg(tag, 0, structDecl, "Use syntax like `#[repr \"C\"]`.");
        if (repr != "C") {
            throw DiagnosticError(
                DiagnosticError::Category::Semantic,
                tag ? tag->name.loc : structDecl->loc,
                "unsupported struct repr `" + repr + "`",
                "Only `#[repr \"C\"]` is supported right now.");
        }
        structDecl->setDeclKind(StructDeclKind::ReprC);
        return;
    }

    if (dynamic_cast<AstFuncDecl *>(target)) {
        errorCannotApplyTag(
            tag, target,
            "Use `#[extern \"C\"]` for C ABI functions. The `repr` tag only applies to struct declarations.");
    }
    if (dynamic_cast<AstVarDef *>(target)) {
        errorCannotApplyTag(
            tag, target,
            "The `repr` tag only applies to struct declarations right now.");
    }

    errorCannotApplyTag(
        tag, target,
        "Tags can only be applied to function declarations, struct declarations, and variable definitions.");
}

void
applyBuiltinTagList(AstNode *target, std::vector<AstTag *> *tags) {
    if (!target || !tags) {
        return;
    }

    if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(target)) {
        funcDecl->setAbiKind(AbiKind::Native);
    }
    if (auto *structDecl = dynamic_cast<AstStructDecl *>(target)) {
        structDecl->setDeclKind(StructDeclKind::Native);
    }

    for (auto *tag : *tags) {
        auto name = tagName(tag);
        if (name == "extern") {
            applyExternTag(target, tag);
            continue;
        }
        if (name == "repr") {
            applyReprTag(target, tag);
            continue;
        }
        errorUnknownTag(tag, target);
    }
}

[[noreturn]] void
errorDanglingTags(std::vector<AstTag *> *tags) {
    auto *tag = tags && !tags->empty() ? (*tags)[0] : nullptr;
    throw DiagnosticError(
        DiagnosticError::Category::Semantic,
        tag ? tag->name.loc : location(),
        "tag `" + tagName(tag) +
            "` must be followed by a function declaration, struct declaration, or variable definition",
        "Move the tagged declaration directly below the tag line.");
}

[[noreturn]] void
errorNonTopLevelTag(const AstTagNode *tagNode) {
    auto *tag = tagNode && tagNode->tags && !tagNode->tags->empty()
        ? (*tagNode->tags)[0]
        : nullptr;
    throw DiagnosticError(
        DiagnosticError::Category::Semantic,
        tag ? tag->name.loc : (tagNode ? tagNode->loc : location()),
        "tag `" + tagName(tag) + "` is only allowed on top-level declarations",
        "Move the tagged declaration to module scope. Tags are not supported inside functions, structs, or control-flow blocks.");
}

void
appendPendingTags(std::vector<AstTag *> *&pending, const AstTagNode *tagNode) {
    if (!tagNode || !tagNode->tags) {
        return;
    }
    if (!pending) {
        pending = new std::vector<AstTag *>;
    }
    pending->insert(pending->end(), tagNode->tags->begin(), tagNode->tags->end());
}

AstNode *
applyBuiltinTagsImpl(AstNode *node, bool allowTagsInList) {
    if (!node) {
        return nullptr;
    }
    if (auto *program = dynamic_cast<AstProgram *>(node)) {
        applyBuiltinTagsImpl(program->body, true);
        return program;
    }
    if (auto *list = dynamic_cast<AstStatList *>(node)) {
        std::list<AstNode *> normalized;
        std::vector<AstTag *> *pendingTags = nullptr;
        for (auto *stmt : list->getBody()) {
            if (auto *tagNode = dynamic_cast<AstTagNode *>(stmt)) {
                if (!allowTagsInList) {
                    errorNonTopLevelTag(tagNode);
                }
                appendPendingTags(pendingTags, tagNode);
                continue;
            }

            auto *normalizedStmt = applyBuiltinTagsImpl(stmt, false);
            if (pendingTags) {
                applyBuiltinTagList(normalizedStmt, pendingTags);
                pendingTags = nullptr;
            }
            normalized.push_back(normalizedStmt);
        }
        if (pendingTags) {
            errorDanglingTags(pendingTags);
        }
        list->body = std::move(normalized);
        return list;
    }
    if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(node)) {
        if (funcDecl->body) {
            applyBuiltinTagsImpl(funcDecl->body, false);
        }
        return funcDecl;
    }
    if (auto *structDecl = dynamic_cast<AstStructDecl *>(node)) {
        if (structDecl->body) {
            applyBuiltinTagsImpl(structDecl->body, false);
        }
        return structDecl;
    }
    if (auto *ifNode = dynamic_cast<AstIf *>(node)) {
        applyBuiltinTagsImpl(ifNode->then, false);
        applyBuiltinTagsImpl(ifNode->els, false);
        return ifNode;
    }
    if (auto *forNode = dynamic_cast<AstFor *>(node)) {
        applyBuiltinTagsImpl(forNode->body, false);
        applyBuiltinTagsImpl(forNode->els, false);
        return forNode;
    }
    return node;
}

}  // namespace tag_apply_impl

AstNode *
applyBuiltinTags(AstNode *node) {
    return tag_apply_impl::applyBuiltinTagsImpl(node, true);
}

void
validateBuiltinTagResults(AstNode *node) {
    if (!node) {
        return;
    }
    if (auto *program = dynamic_cast<AstProgram *>(node)) {
        validateBuiltinTagResults(program->body);
        return;
    }
    if (auto *list = dynamic_cast<AstStatList *>(node)) {
        for (auto *stmt : list->getBody()) {
            validateBuiltinTagResults(stmt);
        }
        return;
    }
    if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(node)) {
        if (funcDecl->body) {
            validateBuiltinTagResults(funcDecl->body);
        }
        return;
    }
    if (auto *structDecl = dynamic_cast<AstStructDecl *>(node)) {
        if (!structDecl->hasBody()) {
            if (structDecl->isReprC()) {
                throw DiagnosticError(
                    DiagnosticError::Category::Semantic, structDecl->loc,
                    "#[repr \"C\"] struct `" + toStdString(structDecl->name) +
                        "` requires a body",
                    "Use `struct " + toStdString(structDecl->name) +
                        "` for an opaque type, or add fields to `#[repr \"C\"] struct " +
                        toStdString(structDecl->name) + " { ... }`.");
            }
            structDecl->setDeclKind(StructDeclKind::Opaque);
        }
        if (structDecl->hasBody() && structDecl->isOpaqueDecl()) {
            throw DiagnosticError(
                DiagnosticError::Category::Semantic, structDecl->loc,
                "opaque struct `" + toStdString(structDecl->name) +
                    "` cannot declare fields or methods",
                "Use `struct " + toStdString(structDecl->name) +
                    "` for an opaque declaration, or drop the opaque form and keep the body.");
        }
        if (structDecl->body) {
            validateBuiltinTagResults(structDecl->body);
        }
        return;
    }
    if (auto *ifNode = dynamic_cast<AstIf *>(node)) {
        validateBuiltinTagResults(ifNode->then);
        validateBuiltinTagResults(ifNode->els);
        return;
    }
    if (auto *forNode = dynamic_cast<AstFor *>(node)) {
        validateBuiltinTagResults(forNode->body);
        validateBuiltinTagResults(forNode->els);
        return;
    }
}

}  // namespace lona
