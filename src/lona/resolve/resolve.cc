#include "resolve.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
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
namespace resolve_impl {

namespace {

std::vector<string>
collectGenericParamNames(const std::vector<AstToken *> *tokens) {
    std::vector<string> names;
    if (!tokens) {
        return names;
    }
    names.reserve(tokens->size());
    for (auto *token : *tokens) {
        if (token) {
            names.push_back(token->text);
        }
    }
    return names;
}

void
appendGenericParamNames(std::vector<string> &dest,
                        const std::vector<AstToken *> *tokens) {
    if (!tokens) {
        return;
    }
    for (auto *token : *tokens) {
        if (token) {
            dest.push_back(token->text);
        }
    }
}

}  // namespace

class FunctionResolver {
    GlobalScope *global_;
    TypeTable *typeMgr_;
    const CompilationUnit *unit_;
    ResolvedModule &module_;
    ResolvedFunction &resolved_;
    std::unordered_map<string, const ResolvedLocalBinding *> locals_;

    bool hasGenericTypeParam(llvm::StringRef name) const {
        for (const auto &paramName : resolved_.genericTypeParams()) {
            if (paramName == string(name)) {
                return true;
            }
        }
        return false;
    }

    const ModuleInterface::TypeDecl *resolveVisibleTypeDecl(
        const BaseTypeNode *base) const {
        if (!base) {
            return nullptr;
        }
        auto rawName = baseTypeName(base);
        std::string moduleName;
        std::string memberName;
        if (!splitBaseTypeName(base, moduleName, memberName)) {
            if (!unit_) {
                return nullptr;
            }
            auto lookup = unit_->lookupTopLevelName(rawName);
            return lookup.isType() ? lookup.typeDecl : nullptr;
        }

        if (!unit_) {
            return nullptr;
        }
        const auto *imported = unit_->findImportedModule(moduleName);
        if (!imported || !imported->interface) {
            return nullptr;
        }
        auto lookup = unit_->lookupTopLevelName(*imported, memberName);
        return lookup.isType() ? lookup.typeDecl : nullptr;
    }

    TypeClass *resolveConcreteType(TypeNode *node) const {
        return unit_ ? unit_->resolveType(typeMgr_, node)
                     : typeMgr_->getType(node);
    }

    void validateVisibleType(TypeNode *node, const location &loc,
                             const std::string &context) {
        if (!node) {
            return;
        }

        validateTypeNodeLayout(node);
        if (isReservedInitialListTypeNode(node)) {
            errorReservedInitialListType(node->loc);
        }

        if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
            validateVisibleType(param->type, loc, context);
            return;
        }
        if (dynamic_cast<AnyTypeNode *>(node)) {
            return;
        }
        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            auto rawName = baseTypeName(base);
            std::string moduleName;
            std::string memberName;
            if (!splitBaseTypeName(base, moduleName, memberName) &&
                hasGenericTypeParam(rawName)) {
                return;
            }
            if (resolveConcreteType(base)) {
                return;
            }
            error(loc, "unknown type for " + context + ": " + rawName,
                  "Type parameters are only visible inside the generic item "
                  "that declares them.");
        }
        if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
            auto appliedName = describeTypeNode(applied, "<unknown type>");
            auto *base = dynamic_cast<BaseTypeNode *>(applied->base);
            const auto *typeDecl = resolveVisibleTypeDecl(base);
            if (!typeDecl) {
                if (resolveConcreteType(applied)) {
                    return;
                }
                error(loc, "unknown type for " + context + ": " + appliedName,
                      "Type parameters are only visible inside the generic "
                      "item that declares them.");
            }
            if (!typeDecl->isGeneric()) {
                error(applied->loc,
                      "type `" + appliedName +
                          "` applies `![...]` arguments to a non-generic type",
                      "Remove the `![...]` arguments, or make the base type "
                      "generic before specializing it.");
            }
            if (applied->args.size() != typeDecl->typeParams.size()) {
                error(applied->loc,
                      "generic type argument count mismatch for `" +
                          toStdString(typeDecl->exportedName) + "`: expected " +
                          std::to_string(typeDecl->typeParams.size()) +
                          ", got " +
                          std::to_string(applied->args.size()),
                      "Match the number of `![` `]` type arguments to the "
                      "generic type parameter list.");
            }
            for (auto *arg : applied->args) {
                validateVisibleType(arg, arg ? arg->loc : loc,
                                    "type argument for `" + appliedName + "`");
            }
            return;
        }
        if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
            validateVisibleType(qualified->base, loc, context);
            return;
        }
        if (auto *dynType = dynamic_cast<DynTypeNode *>(node)) {
            if (resolveConcreteType(dynType)) {
                return;
            }
            error(loc,
                  "unknown type for " + context + ": " +
                      describeTypeNode(dynType, "void"),
                  "Type parameters are only visible inside the generic item "
                  "that declares them.");
        }
        if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
            validateVisibleType(pointer->base, loc, context);
            return;
        }
        if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
            validateVisibleType(indexable->base, loc, context);
            return;
        }
        if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
            validateVisibleType(array->base, loc, context);
            return;
        }
        if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
            for (auto *item : tuple->items) {
                validateVisibleType(item, item ? item->loc : loc, context);
            }
            return;
        }
        if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
            for (auto *arg : func->args) {
                validateVisibleType(arg, arg ? arg->loc : loc, context);
            }
            validateVisibleType(func->ret, loc, context);
            return;
        }
        if (resolveConcreteType(node)) {
            return;
        }
        error(loc,
              "unknown type for " + context + ": " +
                  describeTypeNode(node, "void"),
              "Type parameters are only visible inside the generic item "
              "that declares them.");
    }

    void declareBinding(const ResolvedLocalBinding *binding,
                        const location &loc,
                        const std::string &duplicateMessage,
                        const std::string &duplicateHint) {
        if (unit_ && unit_->importsModule(binding->name())) {
            auto bindingName = toStdString(binding->name());
            error(loc,
                  "local binding `" + bindingName +
                      "` conflicts with imported module alias `" + bindingName +
                      "`",
                  "Rename the local binding so `" + bindingName +
                      ".xxx` continues to refer to the imported module.");
        }
        auto inserted = locals_.emplace(binding->name(), binding);
        if (!inserted.second) {
            error(loc, duplicateMessage, duplicateHint);
        }
    }

    const ResolvedEntityRef *resolvedExpr(const AstNode *node) const {
        if (auto *field = dynamic_cast<const AstField *>(node)) {
            return resolved_.field(field);
        }
        if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
            return resolved_.dotLike(dotLike);
        }
        return nullptr;
    }

    const AstNode *funcRefTargetNode(const AstFuncRef *node) const {
        if (!node) {
            return nullptr;
        }
        if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node->value)) {
            return typeApply->value;
        }
        return node->value;
    }

    const ResolvedEntityRef *resolvedFuncRefTarget(const AstFuncRef *node) const {
        return resolvedExpr(funcRefTargetNode(node));
    }

    std::string describeFuncRefTarget(const AstFuncRef *node) const {
        if (!node) {
            return "<function>";
        }
        if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node->value)) {
            return describeDotLikeSyntax(typeApply->value, "<function>");
        }
        return describeDotLikeSyntax(node->value, "<function>");
    }

    void resolveDotLike(const AstDotLike *node) {
        if (!unit_ || !node) {
            return;
        }

        auto *parentBinding = resolvedExpr(node->parent);
        if (!parentBinding ||
            parentBinding->kind() != ResolvedEntityRef::Kind::Module) {
            return;
        }

        const auto *moduleNamespace =
            unit_->findImportedModule(parentBinding->resolvedName());
        if (!moduleNamespace) {
            internalError(node->loc,
                          "resolved module selector parent is missing from the "
                          "imported-module table",
                          "This looks like a compiler name-resolution bug.");
        }

        auto memberName = toStdString(node->field->text);
        auto lookup = unit_->lookupTopLevelName(*moduleNamespace, memberName);
        if (lookup.isGlobal()) {
            resolved_.bindDotLike(
                node, ResolvedEntityRef::globalValue(lookup.resolvedName));
            return;
        }
        if (lookup.isFunction()) {
            if (lookup.functionDecl && lookup.functionDecl->isGeneric()) {
                resolved_.bindDotLike(
                    node, ResolvedEntityRef::genericFunction(
                              lookup.resolvedName, lookup.functionDecl,
                              moduleNamespace->interface));
                return;
            }
            resolved_.bindDotLike(
                node, ResolvedEntityRef::globalValue(lookup.resolvedName));
            return;
        }
        if (lookup.isType()) {
            if (lookup.typeDecl && lookup.typeDecl->isGeneric()) {
                resolved_.bindDotLike(
                    node, ResolvedEntityRef::genericType(lookup.resolvedName,
                                                         lookup.typeDecl,
                                                         moduleNamespace->interface));
            } else {
                resolved_.bindDotLike(
                    node, ResolvedEntityRef::type(lookup.resolvedName));
            }
            return;
        }
        if (lookup.isTrait()) {
            resolved_.bindDotLike(
                node, ResolvedEntityRef::trait(lookup.resolvedName));
            return;
        }

        error(node->loc,
              "unknown module member `" +
                  toStdString(parentBinding->resolvedName()) + "." +
                  memberName + "`",
              "Only directly imported top-level functions, globals, types, "
              "and traits are available through `file.xxx`.");
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
            if (resolved_.isTemplateValidationOnly() && varDef->getTypeNode()) {
                validateVisibleType(varDef->getTypeNode(),
                                    varDef->getTypeNode()->loc,
                                    "local variable `" +
                                        toStdString(varDef->getName()) + "`");
            }
            if (varDef->withInitVal()) {
                resolveExpr(varDef->getInitVal());
            }
            auto *binding = module_.createLocalBinding(
                ResolvedLocalBinding::Kind::Variable, varDef->getBindingKind(),
                toStdString(varDef->getName()), varDef, varDef->loc);
            declareBinding(
                binding, varDef->loc,
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
        if (node->is<AstStructDecl>() || node->is<AstTraitDecl>() ||
            node->is<AstTraitImplDecl>() || node->is<AstFuncDecl>() ||
            node->is<AstGlobalDecl>() || node->is<AstImport>()) {
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
            auto local = locals_.find(field->name);
            if (local != locals_.end()) {
                resolved_.bindField(field,
                                    ResolvedEntityRef::local(local->second));
                return;
            }

            if (unit_) {
                auto lookup =
                    unit_->lookupTopLevelName(toStdString(field->name));
                if (lookup.isFunction()) {
                    if (lookup.functionDecl && lookup.functionDecl->isGeneric()) {
                        resolved_.bindField(field,
                                            ResolvedEntityRef::genericFunction(
                                                lookup.resolvedName,
                                                lookup.functionDecl));
                    } else {
                        resolved_.bindField(
                            field, ResolvedEntityRef::globalValue(
                                       lookup.resolvedName));
                    }
                    return;
                }
                if (lookup.isGlobal()) {
                    resolved_.bindField(field, ResolvedEntityRef::globalValue(
                                                   lookup.resolvedName));
                    return;
                }
                if (lookup.isType()) {
                    if (lookup.typeDecl && lookup.typeDecl->isGeneric()) {
                        resolved_.bindField(
                            field, ResolvedEntityRef::genericType(
                                       lookup.resolvedName, lookup.typeDecl));
                    } else {
                        resolved_.bindField(
                            field,
                            ResolvedEntityRef::type(lookup.resolvedName));
                    }
                    return;
                }
                if (lookup.isTrait()) {
                    resolved_.bindField(
                        field, ResolvedEntityRef::trait(lookup.resolvedName));
                    return;
                }
                if (lookup.isModule()) {
                    resolved_.bindField(
                        field, ResolvedEntityRef::module(lookup.resolvedName));
                    return;
                }
            }

            auto *globalObject = global_->getObj(field->name);
            if (!globalObject) {
                auto *globalType =
                    typeMgr_ ? typeMgr_->getType(llvm::StringRef(
                                   field->name.tochara(), field->name.size()))
                             : nullptr;
                if (globalType) {
                    resolved_.bindField(
                        field, ResolvedEntityRef::type(
                                   toStdString(globalType->full_name)));
                    return;
                }
                error(field->loc,
                      "undefined identifier `" + toStdString(field->name) + "`",
                      "Declare it with `var` before using it, or check the "
                      "spelling.");
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
            resolved_.bindField(field, ResolvedEntityRef::globalValue(
                                           toStdString(field->name)));
            return;
        }
        if (auto *funcRef = dynamic_cast<const AstFuncRef *>(node)) {
            resolveExpr(funcRef->value);
            if (auto *binding = resolvedFuncRefTarget(funcRef)) {
                if (binding->kind() == ResolvedEntityRef::Kind::GenericFunction ||
                    binding->kind() == ResolvedEntityRef::Kind::GlobalValue) {
                    resolved_.bindFunctionRef(funcRef, *binding);
                    return;
                }
            }
            error(funcRef->loc,
                  "function reference target must name a top-level function: `" +
                      describeFuncRefTarget(funcRef) + "`",
                  "Use `name&<...>` or `module.name&<...>` with a visible top-level "
                  "function.");
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
        if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node)) {
            resolveExpr(typeApply->value);
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
            if (resolved_.isTemplateValidationOnly()) {
                validateVisibleType(castExpr->targetType,
                                    castExpr->targetType->loc,
                                    "cast target type");
            }
            resolveExpr(castExpr->value);
            return;
        }
        if (auto *sizeofExpr = dynamic_cast<const AstSizeofExpr *>(node)) {
            if (resolved_.isTemplateValidationOnly() &&
                sizeofExpr->targetType) {
                validateVisibleType(sizeofExpr->targetType,
                                    sizeofExpr->targetType->loc,
                                    "`sizeof[...]` target type");
            }
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
                     const CompilationUnit *unit, ResolvedModule &module,
                     ResolvedFunction &resolved)
        : global_(global),
          typeMgr_(typeMgr),
          unit_(unit),
          module_(module),
          resolved_(resolved) {}

    void resolve() {
        if (resolved_.hasSelfBinding()) {
            declareBinding(resolved_.selfBinding(), resolved_.loc(),
                           "duplicate implicit `self` binding",
                           "Rename the colliding parameter or variable.");
        }

        for (auto *binding : resolved_.params()) {
            declareBinding(
                binding, binding->loc(),
                "duplicate function parameter `" +
                    toStdString(binding->name()) + "`",
                "Rename one of the parameters so each binding is unique.");
        }

        resolveStmt(resolved_.body());
    }
};

class ModuleResolver {
    GlobalScope *global_;
    TypeTable *typeMgr_;
    const CompilationUnit *unit_;
    bool rootModule_ = false;
    std::unique_ptr<ResolvedModule> module_ =
        std::make_unique<ResolvedModule>();

    ResolvedFunction *createResolvedFunction(
        const AstFuncDecl *decl, const AstNode *body, string functionName,
        string methodParentTypeName, const location &loc, bool topLevelEntry,
        bool languageEntry, bool guaranteedReturn,
        bool templateValidationOnly = false,
        std::vector<string> genericTypeParams = {},
        std::unordered_map<std::string, TypeClass *> concreteGenericTypes = {}) {
        auto *resolved = module_->createFunction(
            decl, body, std::move(functionName),
            std::move(methodParentTypeName), loc, topLevelEntry, languageEntry,
            guaranteedReturn, templateValidationOnly,
            std::move(genericTypeParams),
            std::move(concreteGenericTypes));
        if (resolved->isMethod()) {
            resolved->setSelfBinding(module_->createLocalBinding(
                ResolvedLocalBinding::Kind::Self, BindingKind::Value, "self",
                decl, loc));
        }
        if (decl && decl->args) {
            for (auto *arg : *decl->args) {
                auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
                if (!varDecl) {
                    error(decl->loc, "invalid function argument declaration",
                          "Each function parameter must be declared as a typed "
                          "variable.");
                }
                resolved->addParam(module_->createLocalBinding(
                    ResolvedLocalBinding::Kind::Parameter, varDecl->bindingKind,
                    toStdString(varDecl->field), varDecl, varDecl->loc));
            }
        }
        return resolved;
    }

    void resolveFunction(AstFuncDecl *node, StructType *methodParent = nullptr,
                         string methodParentTypeName = string(),
                         std::vector<string> scopedTypeParams = {}) {
        if (!node) {
            return;
        }
        appendGenericParamNames(scopedTypeParams, node->typeParams);
        const bool templateValidationOnly = !scopedTypeParams.empty();
        Function *function = nullptr;
        string resolvedFunctionName = node->name;
        if (!templateValidationOnly && methodParent) {
            function = typeMgr_->getMethodFunction(
                methodParent,
                llvm::StringRef(node->name.tochara(), node->name.size()));
        } else if (!templateValidationOnly) {
            if (unit_) {
                auto lookup = unit_->lookupTopLevelName(node->name);
                if (lookup.isFunction()) {
                    resolvedFunctionName = lookup.resolvedName;
                }
            }
            auto *obj = global_->getObj(resolvedFunctionName);
            function = obj ? obj->as<Function>() : nullptr;
        }
        if (!templateValidationOnly && !function) {
            error(node->loc,
                  "function declaration is missing from the symbol table",
                  "Run declaration collection before name resolution.");
        }

        auto *resolved = createResolvedFunction(
            node, node->body,
            methodParent ? string(node->name)
                         : (templateValidationOnly ? string()
                                                   : resolvedFunctionName),
            !methodParentTypeName.empty()
                ? std::move(methodParentTypeName)
                : (methodParent ? string(methodParent->full_name) : string()),
            node->loc, false, false, node->body && node->body->hasTerminator(),
            templateValidationOnly, std::move(scopedTypeParams));
        FunctionResolver(global_, typeMgr_, unit_, *module_, *resolved)
            .resolve();
    }

    void resolveStruct(AstStructDecl *node) {
        if (!node) {
            return;
        }
        string resolvedStructName = node->name;
        if (unit_) {
            auto lookup = unit_->lookupTopLevelName(resolvedStructName);
            if (lookup.isType()) {
                resolvedStructName = lookup.resolvedName;
            }
        }
        StructType *structType = nullptr;
        if (!node->hasTypeParams()) {
            auto *type = typeMgr_->getType(resolvedStructName);
            structType = type ? type->as<StructType>() : nullptr;
        }
        if (!node->hasTypeParams() && !structType) {
            error(node->loc,
                  "struct declaration is missing from the type table",
                  "Run type scanning before name resolution.");
        }
        auto *body = dynamic_cast<AstStatList *>(node->body);
        if (!body) {
            return;
        }
        auto scopedTypeParams = collectGenericParamNames(node->typeParams);
        for (auto *stmt : body->getBody()) {
            auto *func = dynamic_cast<AstFuncDecl *>(stmt);
            if (func) {
                resolveFunction(func, structType, resolvedStructName,
                                scopedTypeParams);
            }
        }
    }

    void resolveTopLevel(AstStatList *body) {
        auto *execBody = new AstStatList();
        bool hasImports = false;
        for (auto *stmt : body->getBody()) {
            if (auto *structDecl = dynamic_cast<AstStructDecl *>(stmt)) {
                resolveStruct(structDecl);
                continue;
            }
            if (dynamic_cast<AstImport *>(stmt)) {
                hasImports = true;
                continue;
            }
            if (dynamic_cast<AstGlobalDecl *>(stmt)) {
                continue;
            }
            if (dynamic_cast<AstTraitDecl *>(stmt) ||
                dynamic_cast<AstTraitImplDecl *>(stmt)) {
                continue;
            }
            if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt)) {
                resolveFunction(funcDecl);
                continue;
            }
            execBody->push(stmt);
        }

        const bool shouldCreateTopLevelEntry =
            !rootModule_ || !execBody->isEmpty() || hasImports;
        if (!shouldCreateTopLevelEntry) {
            return;
        }

        auto *resolved = createResolvedFunction(
            nullptr, execBody, std::string(), std::string(), execBody->loc,
            true, rootModule_, execBody->hasTerminator());
        FunctionResolver(global_, typeMgr_, unit_, *module_, *resolved)
            .resolve();
    }

public:
    explicit ModuleResolver(GlobalScope *global, const CompilationUnit *unit,
                            bool rootModule)
        : global_(global),
          typeMgr_(global->types()),
          unit_(unit),
          rootModule_(rootModule) {
        assert(typeMgr_);
    }

    std::unique_ptr<ResolvedModule> resolve(AstNode *root) {
        auto *program = dynamic_cast<AstProgram *>(root);
        auto *body =
            dynamic_cast<AstStatList *>(program ? program->body : root);
        if (!body) {
            error("program body must be a statement list");
        }
        resolveTopLevel(body);
        return std::move(module_);
    }
};

}  // namespace resolve_impl

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
                                   BindingKind bindingKind, string name,
                                   const AstNode *node, const location &loc) {
    localBindings_.push_back(std::make_unique<ResolvedLocalBinding>(
        kind, bindingKind, std::move(name), node, loc));
    return localBindings_.back().get();
}

ResolvedFunction *
ResolvedModule::createFunction(const AstFuncDecl *decl, const AstNode *body,
                               string functionName, string methodParentTypeName,
                               const location &loc, bool topLevelEntry,
                               bool languageEntry, bool guaranteedReturn,
                               bool templateValidationOnly,
                               std::vector<string> genericTypeParams,
                               std::unordered_map<std::string, TypeClass *>
                                   concreteGenericTypes) {
    functions_.push_back(std::make_unique<ResolvedFunction>(
        decl, body, std::move(functionName), std::move(methodParentTypeName),
        loc, topLevelEntry, languageEntry, guaranteedReturn,
        templateValidationOnly, std::move(genericTypeParams),
        std::move(concreteGenericTypes)));
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
resolveModule(GlobalScope *global, AstNode *root, const CompilationUnit *unit,
              bool rootModule) {
    return resolve_impl::ModuleResolver(global, unit, rootModule).resolve(root);
}

std::unique_ptr<ResolvedModule>
resolveGenericFunctionInstance(
    GlobalScope *global, const CompilationUnit *unit, const AstFuncDecl *decl,
    string resolvedFunctionName,
    std::unordered_map<std::string, TypeClass *> concreteGenericTypes) {
    if (!global || !decl) {
        return nullptr;
    }

    auto *typeMgr = global->types();
    assert(typeMgr);

    std::vector<string> genericTypeParams;
    if (decl->typeParams) {
        genericTypeParams.reserve(decl->typeParams->size());
        for (auto *token : *decl->typeParams) {
            if (token) {
                genericTypeParams.push_back(token->text);
            }
        }
    }

    auto module = std::make_unique<ResolvedModule>();
    auto *resolved = module->createFunction(
        decl, decl->body, std::move(resolvedFunctionName), string(), decl->loc,
        false, false, decl->body && decl->body->hasTerminator(), false,
        std::move(genericTypeParams),
        std::move(concreteGenericTypes));

    if (decl->args) {
        for (auto *arg : *decl->args) {
            auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
            if (!varDecl) {
                error(decl->loc, "invalid function argument declaration",
                      "Each function parameter must be declared as a typed "
                      "variable.");
            }
            resolved->addParam(module->createLocalBinding(
                ResolvedLocalBinding::Kind::Parameter, varDecl->bindingKind,
                toStdString(varDecl->field), varDecl, varDecl->loc));
        }
    }

    resolve_impl::FunctionResolver(global, typeMgr, unit, *module, *resolved)
        .resolve();
    return module;
}

std::unique_ptr<ResolvedModule>
resolveGenericMethodInstance(
    GlobalScope *global, const CompilationUnit *unit, const AstFuncDecl *decl,
    string methodParentTypeName, std::vector<string> genericTypeParams,
    std::unordered_map<std::string, TypeClass *> concreteGenericTypes) {
    if (!global || !decl || methodParentTypeName.empty()) {
        return nullptr;
    }

    auto *typeMgr = global->types();
    assert(typeMgr);

    auto module = std::make_unique<ResolvedModule>();
    auto *resolved = module->createFunction(
        decl, decl->body, string(decl->name), std::move(methodParentTypeName),
        decl->loc, false, false, decl->body && decl->body->hasTerminator(),
        false, std::move(genericTypeParams),
        std::move(concreteGenericTypes));

    auto *declStructType =
        typeMgr->getType(resolved->methodParentTypeName())->as<StructType>();
    if (!declStructType) {
        throw DiagnosticError(
            DiagnosticError::Category::Internal,
            "generic method instance is missing its concrete parent type",
            "This looks like a generic method instantiation bug.");
    }

    resolved->setSelfBinding(module->createLocalBinding(
        ResolvedLocalBinding::Kind::Self, BindingKind::Value, "self", decl,
        decl->loc));

    if (decl->args) {
        for (auto *arg : *decl->args) {
            auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
            if (!varDecl) {
                error(decl->loc, "invalid function argument declaration",
                      "Each function parameter must be declared as a typed "
                      "variable.");
            }
            resolved->addParam(module->createLocalBinding(
                ResolvedLocalBinding::Kind::Parameter, varDecl->bindingKind,
                toStdString(varDecl->field), varDecl, varDecl->loc));
        }
    }

    resolve_impl::FunctionResolver(global, typeMgr, unit, *module, *resolved)
        .resolve();
    return module;
}

}  // namespace lona
