#include "resolve.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/sym/func.hh"
#include "lona/sym/object.hh"
#include "lona/type/scope.hh"
#include "lona/type/type.hh"
#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace lona {
namespace {

std::string
toStdString(const string &value) {
    return std::string(value.tochara(), value.size());
}

[[noreturn]] void
error(const std::string &message) {
    throw DiagnosticError(DiagnosticError::Category::Semantic, message);
}

[[noreturn]] void
error(const location &loc, const std::string &message,
      const std::string &hint = std::string()) {
    throw DiagnosticError(DiagnosticError::Category::Semantic, loc, message, hint);
}

[[noreturn]] void
internalError(const location &loc, const std::string &message,
              const std::string &hint = std::string()) {
    throw DiagnosticError(DiagnosticError::Category::Internal, loc, message, hint);
}

class FunctionResolver {
    GlobalScope *global_;
    TypeTable *typeMgr_;
    const CompilationUnit *unit_;
    ResolvedModule &module_;
    ResolvedFunction &resolved_;
    std::unordered_map<std::string, const ResolvedLocalBinding *> locals_;

    void declareBinding(const ResolvedLocalBinding *binding, const location &loc,
                        const std::string &duplicateMessage,
                        const std::string &duplicateHint) {
        if (unit_ && unit_->importsModule(binding->name())) {
            error(loc,
                  "local binding `" + binding->name() +
                      "` conflicts with imported module alias `" +
                      binding->name() + "`",
                  "Rename the local binding so `" + binding->name() +
                      ".xxx` continues to refer to the imported module.");
        }
        auto inserted = locals_.emplace(binding->name(), binding);
        if (!inserted.second) {
            error(loc, duplicateMessage, duplicateHint);
        }
    }

    const ResolvedEntityRef *
    resolvedExpr(const AstNode *node) const {
        if (auto *field = dynamic_cast<const AstField *>(node)) {
            return resolved_.field(field);
        }
        if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
            return resolved_.dotLike(dotLike);
        }
        return nullptr;
    }

    void
    resolveDotLike(const AstDotLike *node) {
        if (!unit_ || !node) {
            return;
        }

        auto *parentBinding = resolvedExpr(node->parent);
        if (!parentBinding || parentBinding->kind() != ResolvedEntityRef::Kind::Module) {
            return;
        }

        const auto *moduleNamespace = unit_->findImportedModule(parentBinding->resolvedName());
        if (!moduleNamespace) {
            internalError(
                node->loc,
                "resolved module selector parent is missing from the imported-module table",
                "This looks like a compiler name-resolution bug.");
        }

        auto memberName = toStdString(node->field->text);
        auto lookup = unit_->lookupTopLevelName(*moduleNamespace, memberName);
        if (lookup.isFunction()) {
            resolved_.bindDotLike(node, ResolvedEntityRef::globalValue(lookup.resolvedName));
            return;
        }
        if (lookup.isType()) {
            resolved_.bindDotLike(node, ResolvedEntityRef::type(lookup.resolvedName));
            return;
        }

        error(node->loc,
              "unknown module member `" + parentBinding->resolvedName() + "." +
                  memberName + "`",
              "Only directly imported top-level functions and types are available through `file.xxx`.");
    }

    void resolveStmt(const AstNode *node) {
        if (!node) {
            return;
        }
        if (auto *list = dynamic_cast<const AstStatList *>(node)) {
            for (auto *stmt : list->body) {
                resolveStmt(stmt);
            }
            return;
        }
        if (auto *varDef = dynamic_cast<const AstVarDef *>(node)) {
            if (varDef->withInitVal()) {
                resolveExpr(varDef->getInitVal());
            }
            auto *binding = module_.createLocalBinding(
                ResolvedLocalBinding::Kind::Variable,
                varDef->getBindingKind(),
                toStdString(varDef->getName()), varDef, varDef->loc);
            declareBinding(binding, varDef->loc,
                           "duplicate variable definition for `" +
                               toStdString(varDef->getName()) + "`",
                           "Rename one of the variables or reuse the existing binding.");
            resolved_.bindVariable(varDef, binding);
            return;
        }
        if (auto *ret = dynamic_cast<const AstRet *>(node)) {
            if (ret->expr) {
                resolveExpr(ret->expr);
            }
            return;
        }
        if (dynamic_cast<const AstBreak *>(node) ||
            dynamic_cast<const AstContinue *>(node)) {
            return;
        }
        if (auto *ifNode = dynamic_cast<const AstIf *>(node)) {
            resolveExpr(ifNode->condition);
            resolveStmt(ifNode->then);
            if (ifNode->els) {
                resolveStmt(ifNode->els);
            }
            return;
        }
        if (auto *forNode = dynamic_cast<const AstFor *>(node)) {
            resolveExpr(forNode->expr);
            resolveStmt(forNode->body);
            if (forNode->els) {
                resolveStmt(forNode->els);
            }
            return;
        }
        if (node->is<AstStructDecl>() || node->is<AstFuncDecl>() ||
            node->is<AstImport>()) {
            return;
        }
        resolveExpr(node);
    }

    void resolveExpr(const AstNode *node) {
        if (!node) {
            return;
        }
        if (dynamic_cast<const AstConst *>(node)) {
            return;
        }
        if (auto *field = dynamic_cast<const AstField *>(node)) {
            auto local = locals_.find(toStdString(field->name));
            if (local != locals_.end()) {
                resolved_.bindField(field, ResolvedEntityRef::local(local->second));
                return;
            }

            if (unit_) {
                auto lookup = unit_->lookupTopLevelName(toStdString(field->name));
                if (lookup.isFunction()) {
                    resolved_.bindField(field,
                                        ResolvedEntityRef::globalValue(
                                            lookup.resolvedName));
                    return;
                }
                if (lookup.isType()) {
                    resolved_.bindField(field,
                                        ResolvedEntityRef::type(lookup.resolvedName));
                    return;
                }
                if (lookup.isModule()) {
                    resolved_.bindField(field,
                                        ResolvedEntityRef::module(
                                            lookup.resolvedName));
                    return;
                }
            }

            auto *globalObject = global_->getObj(field->name);
            if (!globalObject) {
                auto *globalType = typeMgr_
                    ? typeMgr_->getType(llvm::StringRef(field->name.tochara(),
                                                        field->name.size()))
                    : nullptr;
                if (globalType) {
                    resolved_.bindField(field,
                                        ResolvedEntityRef::type(
                                            toStdString(globalType->full_name)));
                    return;
                }
                error(field->loc,
                      "undefined identifier `" + toStdString(field->name) + "`",
                      "Declare it with `var` before using it, or check the spelling.");
            }
            if (globalObject->as<ModuleObject>() &&
                (!unit_ || !unit_->importsModule(toStdString(field->name)))) {
                error(field->loc,
                      "module `" + toStdString(field->name) +
                          "` is not directly imported here",
                      "Add an explicit `import " + toStdString(field->name) +
                          "` in this file before using `" +
                          toStdString(field->name) + ".xxx`.");
            }
            resolved_.bindField(field,
                                ResolvedEntityRef::globalValue(
                                    toStdString(field->name)));
            return;
        }
        if (auto *funcRef = dynamic_cast<const AstFuncRef *>(node)) {
            if (unit_) {
                auto lookup = unit_->lookupTopLevelName(toStdString(funcRef->name));
                if (lookup.isFunction()) {
                    resolved_.bindFunctionRef(
                        funcRef, ResolvedEntityRef::globalValue(lookup.resolvedName));
                    return;
                }
            }
            auto *obj = global_->getObj(funcRef->name);
            auto *func = obj ? obj->as<Function>() : nullptr;
            if (!func) {
                error(funcRef->loc,
                      "undefined function reference `" +
                          toStdString(funcRef->name) + "`",
                      "Check the function name and make sure it is declared at top level.");
            }
            resolved_.bindFunctionRef(
                funcRef, ResolvedEntityRef::globalValue(toStdString(funcRef->name)));
            return;
        }
        if (auto *assign = dynamic_cast<const AstAssign *>(node)) {
            resolveExpr(assign->left);
            resolveExpr(assign->right);
            return;
        }
        if (auto *bin = dynamic_cast<const AstBinOper *>(node)) {
            resolveExpr(bin->left);
            resolveExpr(bin->right);
            return;
        }
        if (auto *unary = dynamic_cast<const AstUnaryOper *>(node)) {
            resolveExpr(unary->expr);
            return;
        }
        if (auto *refExpr = dynamic_cast<const AstRefExpr *>(node)) {
            resolveExpr(refExpr->expr);
            return;
        }
        if (auto *tuple = dynamic_cast<const AstTupleLiteral *>(node)) {
            if (tuple->items) {
                for (auto *item : *tuple->items) {
                    resolveExpr(item);
                }
            }
            return;
        }
        if (auto *braceItem = dynamic_cast<const AstBraceInitItem *>(node)) {
            if (braceItem->value) {
                resolveExpr(braceItem->value);
            }
            return;
        }
        if (auto *braceInit = dynamic_cast<const AstBraceInit *>(node)) {
            if (braceInit->items) {
                for (auto *item : *braceInit->items) {
                    resolveExpr(item);
                }
            }
            return;
        }
        if (auto *namedArg = dynamic_cast<const AstNamedCallArg *>(node)) {
            if (namedArg->value) {
                resolveExpr(namedArg->value);
            }
            return;
        }
        if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
            resolveExpr(dotLike->parent);
            resolveDotLike(dotLike);
            return;
        }
        if (auto *castExpr = dynamic_cast<const AstCastExpr *>(node)) {
            resolveExpr(castExpr->value);
            return;
        }
        if (auto *sizeofExpr = dynamic_cast<const AstSizeofExpr *>(node)) {
            if (sizeofExpr->value) {
                resolveExpr(sizeofExpr->value);
            }
            return;
        }
        if (auto *call = dynamic_cast<const AstFieldCall *>(node)) {
            resolveExpr(call->value);
            if (call->args) {
                for (auto *arg : *call->args) {
                    resolveExpr(arg);
                }
            }
            return;
        }
        if (auto *list = dynamic_cast<const AstStatList *>(node)) {
            resolveStmt(list);
            return;
        }
        error(node->loc, "unsupported AST node in name resolution",
              "This looks like a compiler bug in the frontend pipeline.");
    }

public:
    FunctionResolver(GlobalScope *global, TypeTable *typeMgr,
                     const CompilationUnit *unit,
                     ResolvedModule &module,
                     ResolvedFunction &resolved)
        : global_(global), typeMgr_(typeMgr), unit_(unit), module_(module),
          resolved_(resolved) {}

    void resolve() {
        if (resolved_.hasSelfBinding()) {
            declareBinding(resolved_.selfBinding(), resolved_.loc(),
                           "duplicate implicit `self` binding",
                           "Rename the colliding parameter or variable.");
        }

        for (auto *binding : resolved_.params()) {
            declareBinding(binding, binding->loc(),
                           "duplicate function parameter `" + binding->name() + "`",
                           "Rename one of the parameters so each binding is unique.");
        }

        resolveStmt(resolved_.body());
    }
};

class ModuleResolver {
    GlobalScope *global_;
    TypeTable *typeMgr_;
    const CompilationUnit *unit_;
    std::unique_ptr<ResolvedModule> module_ = std::make_unique<ResolvedModule>();

    ResolvedFunction *createResolvedFunction(const AstFuncDecl *decl, const AstNode *body,
                                             std::string functionName,
                                             std::string methodParentTypeName,
                                             const location &loc,
                                             bool topLevelEntry,
                                             bool guaranteedReturn) {
        auto *resolved = module_->createFunction(
            decl, body, std::move(functionName), std::move(methodParentTypeName), loc,
            topLevelEntry, guaranteedReturn);
        if (resolved->isMethod()) {
            resolved->setSelfBinding(module_->createLocalBinding(
                ResolvedLocalBinding::Kind::Self, BindingKind::Ref, "self", decl, loc));
        }
        if (decl && decl->args) {
            for (auto *arg : *decl->args) {
                auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
                if (!varDecl) {
                    error(decl->loc,
                          "invalid function argument declaration",
                          "Each function parameter must be declared as a typed variable.");
                }
                resolved->addParam(module_->createLocalBinding(
                    ResolvedLocalBinding::Kind::Parameter,
                    varDecl->bindingKind,
                    toStdString(varDecl->field), varDecl, varDecl->loc));
            }
        }
        return resolved;
    }

    void resolveFunction(AstFuncDecl *node, StructType *methodParent = nullptr) {
        Function *function = nullptr;
        auto resolvedFunctionName = toStdString(node->name);
        if (methodParent) {
            function = typeMgr_->getMethodFunction(
                methodParent,
                llvm::StringRef(node->name.tochara(), node->name.size()));
        } else {
            if (unit_) {
                auto lookup = unit_->lookupTopLevelName(toStdString(node->name));
                if (lookup.isFunction()) {
                    resolvedFunctionName = lookup.resolvedName;
                }
            }
            auto *obj = global_->getObj(llvm::StringRef(resolvedFunctionName));
            function = obj ? obj->as<Function>() : nullptr;
        }
        if (!function) {
            error(node->loc, "function declaration is missing from the symbol table",
                  "Run declaration collection before name resolution.");
        }

        auto *resolved = createResolvedFunction(
            node, node->body,
            methodParent ? toStdString(node->name) : resolvedFunctionName,
            methodParent ? toStdString(methodParent->full_name) : std::string(),
            node->loc, false,
            node->body && node->body->hasTerminator());
        FunctionResolver(global_, typeMgr_, unit_, *module_, *resolved).resolve();
    }

    void resolveStruct(AstStructDecl *node) {
        auto resolvedStructName = toStdString(node->name);
        if (unit_) {
            auto lookup = unit_->lookupTopLevelName(resolvedStructName);
            if (lookup.isType()) {
                resolvedStructName = lookup.resolvedName;
            }
        }
        auto *type = typeMgr_->getType(llvm::StringRef(resolvedStructName));
        auto *structType = type ? type->as<StructType>() : nullptr;
        if (!structType) {
            error(node->loc, "struct declaration is missing from the type table",
                  "Run type scanning before name resolution.");
        }
        auto *body = dynamic_cast<AstStatList *>(node->body);
        if (!body) {
            return;
        }
        for (auto *stmt : body->getBody()) {
            auto *func = dynamic_cast<AstFuncDecl *>(stmt);
            if (func) {
                resolveFunction(func, structType);
            }
        }
    }

    void resolveTopLevel(AstStatList *body) {
        bool hasTopLevelExec = false;
        for (auto *stmt : body->getBody()) {
            if (auto *structDecl = dynamic_cast<AstStructDecl *>(stmt)) {
                resolveStruct(structDecl);
                continue;
            }
            if (dynamic_cast<AstImport *>(stmt)) {
                continue;
            }
            if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt)) {
                resolveFunction(funcDecl);
                continue;
            }
            hasTopLevelExec = true;
        }

        if (hasTopLevelExec) {
            auto *resolved = createResolvedFunction(
                nullptr, body, std::string(), std::string(), body->loc, true,
                body->hasTerminator());
            FunctionResolver(global_, typeMgr_, unit_, *module_, *resolved).resolve();
        }
    }

public:
    explicit ModuleResolver(GlobalScope *global, const CompilationUnit *unit)
        : global_(global), typeMgr_(global->types()), unit_(unit) {
        assert(typeMgr_);
    }

    std::unique_ptr<ResolvedModule> resolve(AstNode *root) {
        auto *program = dynamic_cast<AstProgram *>(root);
        auto *body = dynamic_cast<AstStatList *>(program ? program->body : root);
        if (!body) {
            error("program body must be a statement list");
        }
        resolveTopLevel(body);
        return std::move(module_);
    }
};

}  // namespace

const ResolvedLocalBinding *
ResolvedFunction::variable(const AstVarDef *node) const {
    auto found = variables_.find(node);
    if (found == variables_.end()) {
        return nullptr;
    }
    return found->second;
}

const ResolvedEntityRef *
ResolvedFunction::field(const AstField *node) const {
    auto found = fields_.find(node);
    if (found == fields_.end()) {
        return nullptr;
    }
    return &found->second;
}

const ResolvedLocalBinding *
ResolvedModule::createLocalBinding(ResolvedLocalBinding::Kind kind,
                                   BindingKind bindingKind,
                                   std::string name, const AstNode *node,
                                   const location &loc) {
    localBindings_.push_back(
        std::make_unique<ResolvedLocalBinding>(kind, bindingKind,
                                               std::move(name), node, loc));
    return localBindings_.back().get();
}

ResolvedFunction *
ResolvedModule::createFunction(const AstFuncDecl *decl, const AstNode *body,
                               std::string functionName,
                               std::string methodParentTypeName,
                               const location &loc, bool topLevelEntry,
                               bool guaranteedReturn) {
    functions_.push_back(std::make_unique<ResolvedFunction>(
        decl, body, std::move(functionName), std::move(methodParentTypeName), loc,
        topLevelEntry,
        guaranteedReturn));
    return functions_.back().get();
}

const ResolvedEntityRef *
ResolvedFunction::dotLike(const AstDotLike *node) const {
    auto found = dotLikes_.find(node);
    if (found == dotLikes_.end()) {
        return nullptr;
    }
    return &found->second;
}

const ResolvedEntityRef *
ResolvedFunction::functionRef(const AstFuncRef *node) const {
    auto found = functionRefs_.find(node);
    if (found == functionRefs_.end()) {
        return nullptr;
    }
    return &found->second;
}

std::unique_ptr<ResolvedModule>
resolveModule(GlobalScope *global, AstNode *root, const CompilationUnit *unit) {
    return ModuleResolver(global, unit).resolve(root);
}

}  // namespace lona
