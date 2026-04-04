#include "resolve.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/sym/func.hh"
#include "lona/sym/object.hh"
#include "lona/type/scope.hh"
#include "lona/type/type.hh"
#include "parser.hh"
#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace lona {
namespace resolve_impl {

namespace {

std::vector<string>
collectGenericParamNames(const std::vector<AstGenericParam *> *tokens) {
    std::vector<string> names;
    if (!tokens) {
        return names;
    }
    names.reserve(tokens->size());
    for (auto *token : *tokens) {
        if (token) {
            names.push_back(token->name.text);
        }
    }
    return names;
}

void
appendGenericParamNames(std::vector<string> &dest,
                        const std::vector<AstGenericParam *> *tokens) {
    if (!tokens) {
        return;
    }
    for (auto *token : *tokens) {
        if (token) {
            dest.push_back(token->name.text);
        }
    }
}

std::unordered_map<std::string, std::string>
collectGenericParamBounds(const std::vector<AstGenericParam *> *tokens) {
    std::unordered_map<std::string, std::string> bounds;
    if (!tokens) {
        return bounds;
    }
    bounds.reserve(tokens->size());
    for (auto *token : *tokens) {
        if (!token || !token->hasBoundTrait()) {
            continue;
        }
        bounds.emplace(toStdString(token->name.text),
                       describeDotLikeSyntax(token->boundTrait, "<trait>"));
    }
    return bounds;
}

}  // namespace

class FunctionResolver {
    GlobalScope *global_;
    TypeTable *typeMgr_;
    const CompilationUnit *unit_;
    ResolvedModule &module_;
    ResolvedFunction &resolved_;
    std::unordered_map<string, const ResolvedLocalBinding *> locals_;
    struct GenericCapabilityInfo {
        string paramName;
        int pointerDepth = -1;

        bool valid() const { return pointerDepth >= 0 && !paramName.empty(); }
        bool isDirectValue() const { return valid() && pointerDepth == 0; }
    };
    std::unordered_map<const ResolvedLocalBinding *, GenericCapabilityInfo>
        bindingGenericInfo_;

    bool hasGenericTypeParam(llvm::StringRef name) const {
        for (const auto &paramName : resolved_.genericTypeParams()) {
            if (paramName == string(name)) {
                return true;
            }
        }
        return false;
    }

    GenericCapabilityInfo noGenericCapability() const { return {}; }

    const AstNode *genericTypeParamBoundSyntax(llvm::StringRef paramName) const {
        auto lookupBound =
            [paramName](const std::vector<AstGenericParam *> *params)
                -> const AstNode * {
            if (!params) {
                return nullptr;
            }
            for (auto *param : *params) {
                if (!param || !param->hasBoundTrait()) {
                    continue;
                }
                if (toStringRef(param->name.text) == paramName) {
                    return param->boundTrait;
                }
            }
            return nullptr;
        };

        if (auto *decl = resolved_.decl()) {
            if (auto *bound = lookupBound(decl->typeParams)) {
                return bound;
            }
            if (auto *structDecl = findEnclosingStructDecl(decl)) {
                if (auto *bound = lookupBound(structDecl->typeParams)) {
                    return bound;
                }
            }
        }
        if (resolved_.isMethod() && unit_) {
            auto structName = toStdString(resolved_.methodParentTypeName());
            if (auto dotPos = structName.rfind('.'); dotPos != std::string::npos) {
                structName = structName.substr(dotPos + 1);
            }
            if (auto *structDecl = findStructDeclInUnit(unit_, structName)) {
                if (auto *bound = lookupBound(structDecl->typeParams)) {
                    return bound;
                }
            }
        }
        return nullptr;
    }

    const AstStructDecl *findStructDeclInUnit(const CompilationUnit *searchUnit,
                                              llvm::StringRef localName) const {
        if (!searchUnit) {
            return nullptr;
        }
        auto *root = searchUnit->syntaxTree();
        auto *program = dynamic_cast<AstProgram *>(root);
        auto *body =
            dynamic_cast<AstStatList *>(program ? program->body : root);
        if (!body) {
            return nullptr;
        }
        for (auto *stmt : body->getBody()) {
            auto *structDecl = dynamic_cast<AstStructDecl *>(stmt);
            if (!structDecl) {
                continue;
            }
            if (llvm::StringRef(structDecl->name.tochara(), structDecl->name.size()) ==
                localName) {
                return structDecl;
            }
        }
        return nullptr;
    }

    const AstStructDecl *findEnclosingStructDecl(const AstFuncDecl *decl) const {
        if (!unit_ || !decl) {
            return nullptr;
        }
        auto *root = unit_->syntaxTree();
        auto *program = dynamic_cast<AstProgram *>(root);
        auto *body =
            dynamic_cast<AstStatList *>(program ? program->body : root);
        if (!body) {
            return nullptr;
        }
        for (auto *stmt : body->getBody()) {
            auto *structDecl = dynamic_cast<AstStructDecl *>(stmt);
            if (!structDecl) {
                continue;
            }
            auto *structBody = dynamic_cast<AstStatList *>(structDecl->body);
            if (!structBody) {
                continue;
            }
            for (auto *member : structBody->getBody()) {
                if (member == decl) {
                    return structDecl;
                }
            }
        }
        return nullptr;
    }

    const AstVarDecl *findStructFieldDecl(const AstStructDecl *structDecl,
                                         llvm::StringRef fieldName) const {
        auto *body = dynamic_cast<AstStatList *>(structDecl ? structDecl->body
                                                            : nullptr);
        if (!body) {
            return nullptr;
        }
        for (auto *stmt : body->getBody()) {
            auto *fieldDecl = dynamic_cast<AstVarDecl *>(stmt);
            if (!fieldDecl) {
                continue;
            }
            if (llvm::StringRef(fieldDecl->field.tochara(),
                                fieldDecl->field.size()) == fieldName) {
                return fieldDecl;
            }
        }
        return nullptr;
    }

    const AstFuncDecl *findStructMethodDecl(const AstStructDecl *structDecl,
                                            llvm::StringRef methodName) const {
        if (!structDecl) {
            return nullptr;
        }
        auto *body = dynamic_cast<AstStatList *>(structDecl->body);
        if (!body) {
            return nullptr;
        }
        for (auto *stmt : body->getBody()) {
            auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt);
            if (!funcDecl) {
                continue;
            }
            if (llvm::StringRef(funcDecl->name.tochara(), funcDecl->name.size()) ==
                methodName) {
                return funcDecl;
            }
        }
        return nullptr;
    }

    const TypeNode *bindingDeclaredTypeNode(
        const ResolvedLocalBinding *binding) const {
        if (!binding) {
            return nullptr;
        }
        if (auto *paramDecl = binding->parameterDecl()) {
            return paramDecl->typeNode;
        }
        if (auto *varDecl = binding->variableDecl()) {
            return varDecl->getTypeNode();
        }
        return nullptr;
    }

    const TypeNode *pointeeTypeNode(const TypeNode *node) const {
        if (!node) {
            return nullptr;
        }
        if (auto *param = dynamic_cast<const FuncParamTypeNode *>(node)) {
            return pointeeTypeNode(param->type);
        }
        if (auto *pointer = dynamic_cast<const PointerTypeNode *>(node)) {
            return pointer->base;
        }
        if (auto *indexable =
                dynamic_cast<const IndexablePointerTypeNode *>(node)) {
            return indexable->base;
        }
        return nullptr;
    }

    const TypeNode *stripDecoratedTypeNode(const TypeNode *node) const {
        while (node) {
            if (auto *param = dynamic_cast<const FuncParamTypeNode *>(node)) {
                node = param->type;
                continue;
            }
            if (auto *qualified = dynamic_cast<const ConstTypeNode *>(node)) {
                node = qualified->base;
                continue;
            }
            break;
        }
        return node;
    }

    const TypeNode *peelPointerTypeNode(const TypeNode *node,
                                        int depth) const {
        auto *current = node;
        for (int i = 0; current && i < depth; ++i) {
            current = pointeeTypeNode(current);
        }
        return current;
    }

    const TypeNode *projectionOwnerTypeNode(const AstNode *expr) const {
        if (!expr) {
            return nullptr;
        }
        if (auto *field = dynamic_cast<const AstField *>(expr)) {
            if (auto *binding = resolved_.field(field)) {
                if (binding->kind() == ResolvedEntityRef::Kind::LocalBinding) {
                    return bindingDeclaredTypeNode(binding->localBinding());
                }
            }
            return nullptr;
        }
        if (auto *refExpr = dynamic_cast<const AstRefExpr *>(expr)) {
            return projectionOwnerTypeNode(refExpr->expr);
        }
        if (auto *unary = dynamic_cast<const AstUnaryOper *>(expr)) {
            if (unary->op != '*') {
                return nullptr;
            }
            auto *inner = projectionOwnerTypeNode(unary->expr);
            return pointeeTypeNode(inner);
        }
        return nullptr;
    }

    bool sameVisibleTypeBase(const BaseTypeNode *lhs,
                             const BaseTypeNode *rhs) const {
        if (!lhs || !rhs) {
            return false;
        }
        if (auto *lhsDecl = resolveVisibleTypeDecl(lhs)) {
            if (auto *rhsDecl = resolveVisibleTypeDecl(rhs)) {
                return lhsDecl == rhsDecl;
            }
        }
        return baseTypeName(lhs) == baseTypeName(rhs);
    }

    GenericCapabilityInfo classifyGenericTypeNode(
        const TypeNode *node,
        const std::unordered_map<std::string, GenericCapabilityInfo> &substs =
            {}) const {
        if (!node) {
            return noGenericCapability();
        }
        if (auto *param = dynamic_cast<const FuncParamTypeNode *>(node)) {
            return classifyGenericTypeNode(param->type, substs);
        }
        if (auto *qualified = dynamic_cast<const ConstTypeNode *>(node)) {
            return classifyGenericTypeNode(qualified->base, substs);
        }
        if (auto *base = dynamic_cast<const BaseTypeNode *>(node)) {
            auto rawName = baseTypeName(base);
            if (auto found = substs.find(rawName); found != substs.end()) {
                return found->second;
            }
            std::string moduleName;
            std::string memberName;
            if (!splitBaseTypeName(base, moduleName, memberName) &&
                resolved_.concreteGenericType(rawName)) {
                return noGenericCapability();
            }
            if (!splitBaseTypeName(base, moduleName, memberName) &&
                hasGenericTypeParam(rawName)) {
                return {rawName, 0};
            }
            return noGenericCapability();
        }
        if (auto *pointer = dynamic_cast<const PointerTypeNode *>(node)) {
            auto info = classifyGenericTypeNode(pointer->base, substs);
            if (info.valid()) {
                info.pointerDepth += static_cast<int>(pointer->dim);
            }
            return info;
        }
        if (auto *indexable =
                dynamic_cast<const IndexablePointerTypeNode *>(node)) {
            auto info = classifyGenericTypeNode(indexable->base, substs);
            if (info.valid()) {
                ++info.pointerDepth;
            }
            return info;
        }
        return noGenericCapability();
    }

    GenericCapabilityInfo projectedFieldGenericInfo(
        const TypeNode *ownerTypeNode, llvm::StringRef fieldName) const {
        if (!unit_ || !ownerTypeNode) {
            return noGenericCapability();
        }
        if (auto *param = dynamic_cast<const FuncParamTypeNode *>(ownerTypeNode)) {
            return projectedFieldGenericInfo(param->type, fieldName);
        }
        if (auto *qualified = dynamic_cast<const ConstTypeNode *>(ownerTypeNode)) {
            return projectedFieldGenericInfo(qualified->base, fieldName);
        }

        const BaseTypeNode *base = nullptr;
        const ModuleInterface::TypeDecl *typeDecl = nullptr;
        std::unordered_map<std::string, GenericCapabilityInfo> substs;
        if (auto *applied = dynamic_cast<const AppliedTypeNode *>(ownerTypeNode)) {
            base = dynamic_cast<const BaseTypeNode *>(applied->base);
            typeDecl = resolveVisibleTypeDecl(base);
            if (!typeDecl) {
                return noGenericCapability();
            }
            const auto count = std::min(typeDecl->typeParams.size(),
                                        applied->args.size());
            for (std::size_t i = 0; i < count; ++i) {
                substs.emplace(toStdString(typeDecl->typeParams[i].localName),
                               classifyGenericTypeNode(applied->args[i]));
            }
        } else if (auto *baseNode =
                       dynamic_cast<const BaseTypeNode *>(ownerTypeNode)) {
            base = baseNode;
            typeDecl = resolveVisibleTypeDecl(baseNode);
        }

        if (!base || !typeDecl) {
            return noGenericCapability();
        }
        auto *ownerUnit = unit_->ownerUnitForTypeDecl(typeDecl);
        auto *structDecl =
            findStructDeclInUnit(ownerUnit, toStringRef(typeDecl->localName));
        auto *fieldDecl = findStructFieldDecl(structDecl, fieldName);
        if (!fieldDecl) {
            return noGenericCapability();
        }
        return classifyGenericTypeNode(fieldDecl->typeNode, substs);
    }

    const TypeNode *methodOwnerTypeNode(const TypeNode *ownerTypeNode) const {
        if (!ownerTypeNode) {
            return nullptr;
        }
        if (auto *param = dynamic_cast<const FuncParamTypeNode *>(ownerTypeNode)) {
            return methodOwnerTypeNode(param->type);
        }
        if (auto *qualified = dynamic_cast<const ConstTypeNode *>(ownerTypeNode)) {
            return methodOwnerTypeNode(qualified->base);
        }
        if (auto *pointer = dynamic_cast<const PointerTypeNode *>(ownerTypeNode)) {
            return stripDecoratedTypeNode(pointer->base);
        }
        if (auto *indexable =
                dynamic_cast<const IndexablePointerTypeNode *>(ownerTypeNode)) {
            return stripDecoratedTypeNode(indexable->base);
        }
        return ownerTypeNode;
    }

    const AstFuncDecl *resolveVisibleMethodDecl(
        const TypeNode *ownerTypeNode, llvm::StringRef methodName,
        std::unordered_map<std::string, GenericCapabilityInfo> *substs =
            nullptr) const {
        if (!unit_ || !ownerTypeNode) {
            return nullptr;
        }
        ownerTypeNode = methodOwnerTypeNode(ownerTypeNode);
        if (!ownerTypeNode) {
            return nullptr;
        }

        const ModuleInterface::TypeDecl *typeDecl = nullptr;
        if (auto *applied = dynamic_cast<const AppliedTypeNode *>(ownerTypeNode)) {
            auto *base = dynamic_cast<const BaseTypeNode *>(applied->base);
            typeDecl = resolveVisibleTypeDecl(base);
            if (!typeDecl) {
                return nullptr;
            }
            if (substs) {
                const auto count =
                    std::min(typeDecl->typeParams.size(), applied->args.size());
                for (std::size_t i = 0; i < count; ++i) {
                    substs->emplace(
                        toStdString(typeDecl->typeParams[i].localName),
                        classifyGenericTypeNode(applied->args[i]));
                }
            }
        } else if (auto *baseNode =
                       dynamic_cast<const BaseTypeNode *>(ownerTypeNode)) {
            typeDecl = resolveVisibleTypeDecl(baseNode);
        }

        if (!typeDecl) {
            return nullptr;
        }
        auto *ownerUnit = unit_->ownerUnitForTypeDecl(typeDecl);
        auto *structDecl =
            findStructDeclInUnit(ownerUnit, toStringRef(typeDecl->localName));
        return findStructMethodDecl(structDecl, methodName);
    }

    const TypeNode *projectedFieldTypeNode(const TypeNode *ownerTypeNode,
                                           llvm::StringRef fieldName) const {
        if (!unit_ || !ownerTypeNode) {
            return nullptr;
        }
        if (auto *param = dynamic_cast<const FuncParamTypeNode *>(ownerTypeNode)) {
            return projectedFieldTypeNode(param->type, fieldName);
        }
        if (auto *qualified = dynamic_cast<const ConstTypeNode *>(ownerTypeNode)) {
            return projectedFieldTypeNode(qualified->base, fieldName);
        }

        const BaseTypeNode *base = nullptr;
        const ModuleInterface::TypeDecl *typeDecl = nullptr;
        if (auto *applied = dynamic_cast<const AppliedTypeNode *>(ownerTypeNode)) {
            base = dynamic_cast<const BaseTypeNode *>(applied->base);
            typeDecl = resolveVisibleTypeDecl(base);
        } else if (auto *baseNode =
                       dynamic_cast<const BaseTypeNode *>(ownerTypeNode)) {
            base = baseNode;
            typeDecl = resolveVisibleTypeDecl(baseNode);
        }

        if (!base || !typeDecl) {
            return nullptr;
        }
        auto *ownerUnit = unit_->ownerUnitForTypeDecl(typeDecl);
        auto *structDecl =
            findStructDeclInUnit(ownerUnit, toStringRef(typeDecl->localName));
        auto *fieldDecl = findStructFieldDecl(structDecl, fieldName);
        return fieldDecl ? fieldDecl->typeNode : nullptr;
    }

    GenericCapabilityInfo selfFieldGenericInfo(llvm::StringRef fieldName) const {
        if (!resolved_.isMethod() || !resolved_.decl()) {
            return noGenericCapability();
        }
        auto *structDecl = findEnclosingStructDecl(resolved_.decl());
        auto *fieldDecl = findStructFieldDecl(structDecl, fieldName);
        if (!fieldDecl) {
            return noGenericCapability();
        }
        std::unordered_map<std::string, GenericCapabilityInfo> identity;
        for (const auto &paramName : resolved_.genericTypeParams()) {
            auto key = toStdString(paramName);
            identity.emplace(key, GenericCapabilityInfo{key, 0});
        }
        return classifyGenericTypeNode(fieldDecl->typeNode, identity);
    }

    const TypeNode *selfFieldTypeNode(llvm::StringRef fieldName) const {
        if (!resolved_.isMethod() || !resolved_.decl()) {
            return nullptr;
        }
        auto *structDecl = findEnclosingStructDecl(resolved_.decl());
        auto *fieldDecl = findStructFieldDecl(structDecl, fieldName);
        return fieldDecl ? fieldDecl->typeNode : nullptr;
    }

    GenericCapabilityInfo bindingGenericInfo(
        const ResolvedLocalBinding *binding) const {
        if (!binding) {
            return noGenericCapability();
        }
        if (auto found = bindingGenericInfo_.find(binding);
            found != bindingGenericInfo_.end()) {
            return found->second;
        }
        return classifyGenericTypeNode(bindingDeclaredTypeNode(binding));
    }

    const TypeNode *exprVisibleTypeNode(const AstNode *node) const {
        if (!node) {
            return nullptr;
        }
        if (auto *field = dynamic_cast<const AstField *>(node)) {
            auto *binding = resolved_.field(field);
            if (!binding ||
                binding->kind() != ResolvedEntityRef::Kind::LocalBinding) {
                return nullptr;
            }
            return bindingDeclaredTypeNode(binding->localBinding());
        }
        if (auto *refExpr = dynamic_cast<const AstRefExpr *>(node)) {
            return exprVisibleTypeNode(refExpr->expr);
        }
        if (auto *unary = dynamic_cast<const AstUnaryOper *>(node)) {
            if (unary->op != '*') {
                return nullptr;
            }
            return pointeeTypeNode(exprVisibleTypeNode(unary->expr));
        }
        if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
            auto fieldName = llvm::StringRef(dotLike->field->text.tochara(),
                                             dotLike->field->text.size());
            if (auto *parentField = dynamic_cast<const AstField *>(dotLike->parent)) {
                if (auto *binding = resolved_.field(parentField);
                    binding &&
                    binding->kind() == ResolvedEntityRef::Kind::LocalBinding &&
                    binding->localBinding() == resolved_.selfBinding()) {
                    return selfFieldTypeNode(fieldName);
                }
            }
            auto *ownerTypeNode = projectionOwnerTypeNode(dotLike->parent);
            return projectedFieldTypeNode(ownerTypeNode, fieldName);
        }
        return nullptr;
    }

    const AstNode *callArgValue(const AstNode *node) const {
        if (auto *namedArg = dynamic_cast<const AstNamedCallArg *>(node)) {
            return namedArg->value;
        }
        return node;
    }

    const AstNode *callTargetNode(const AstFieldCall *node) const {
        if (!node) {
            return nullptr;
        }
        if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node->value)) {
            return typeApply->value;
        }
        return node->value;
    }

    std::vector<TypeNode *> *callExplicitTypeArgs(const AstFieldCall *node) const {
        if (!node) {
            return nullptr;
        }
        if (auto *typeApply = dynamic_cast<const AstTypeApply *>(node->value)) {
            return typeApply->typeArgs;
        }
        return nullptr;
    }

    const ResolvedEntityRef *resolvedCallTarget(const AstFieldCall *node) const {
        return resolvedExpr(callTargetNode(node));
    }

    void recordGenericCapabilitySubst(
        std::unordered_map<std::string, GenericCapabilityInfo> &substs,
        llvm::StringRef paramName, GenericCapabilityInfo info) const {
        if (!info.valid()) {
            return;
        }
        auto key = toStdString(paramName);
        auto found = substs.find(key);
        if (found == substs.end() || !found->second.valid()) {
            substs[key] = std::move(info);
        }
    }

    void inferGenericCapabilitySubsts(
        const TypeNode *pattern, const TypeNode *actualTypeNode,
        GenericCapabilityInfo actualInfo,
        std::unordered_map<std::string, GenericCapabilityInfo> &substs) const {
        if (!pattern) {
            return;
        }
        if (auto *param = dynamic_cast<const FuncParamTypeNode *>(pattern)) {
            inferGenericCapabilitySubsts(param->type, actualTypeNode, actualInfo,
                                         substs);
            return;
        }
        if (auto *qualified = dynamic_cast<const ConstTypeNode *>(pattern)) {
            inferGenericCapabilitySubsts(qualified->base, actualTypeNode,
                                         actualInfo, substs);
            return;
        }
        if (auto *base = dynamic_cast<const BaseTypeNode *>(pattern)) {
            auto rawName = baseTypeName(base);
            std::string moduleName;
            std::string memberName;
            if (!splitBaseTypeName(base, moduleName, memberName) &&
                hasGenericTypeParam(rawName)) {
                auto info = actualInfo;
                if (!info.valid() && actualTypeNode) {
                    info = classifyGenericTypeNode(actualTypeNode);
                }
                recordGenericCapabilitySubst(substs, rawName, info);
            }
            return;
        }
        if (auto *pointer = dynamic_cast<const PointerTypeNode *>(pattern)) {
            auto nextInfo = actualInfo;
            if (nextInfo.valid()) {
                if (nextInfo.pointerDepth >= static_cast<int>(pointer->dim)) {
                    nextInfo.pointerDepth -= static_cast<int>(pointer->dim);
                } else {
                    nextInfo = noGenericCapability();
                }
            }
            inferGenericCapabilitySubsts(pointer->base,
                                         peelPointerTypeNode(actualTypeNode,
                                                             pointer->dim),
                                         nextInfo, substs);
            return;
        }
        if (auto *indexable =
                dynamic_cast<const IndexablePointerTypeNode *>(pattern)) {
            auto nextInfo = actualInfo;
            if (nextInfo.valid()) {
                if (nextInfo.pointerDepth > 0) {
                    --nextInfo.pointerDepth;
                } else {
                    nextInfo = noGenericCapability();
                }
            }
            inferGenericCapabilitySubsts(indexable->base,
                                         peelPointerTypeNode(actualTypeNode, 1),
                                         nextInfo, substs);
            return;
        }
        if (auto *array = dynamic_cast<const ArrayTypeNode *>(pattern)) {
            const auto *actualArray =
                dynamic_cast<const ArrayTypeNode *>(stripDecoratedTypeNode(
                    actualTypeNode));
            inferGenericCapabilitySubsts(
                array->base, actualArray ? actualArray->base : nullptr,
                noGenericCapability(), substs);
            return;
        }
        if (auto *tuple = dynamic_cast<const TupleTypeNode *>(pattern)) {
            const auto *actualTuple =
                dynamic_cast<const TupleTypeNode *>(stripDecoratedTypeNode(
                    actualTypeNode));
            if (!actualTuple || actualTuple->items.size() != tuple->items.size()) {
                return;
            }
            for (std::size_t i = 0; i < tuple->items.size(); ++i) {
                inferGenericCapabilitySubsts(tuple->items[i],
                                             actualTuple->items[i],
                                             noGenericCapability(), substs);
            }
            return;
        }
        if (auto *func = dynamic_cast<const FuncPtrTypeNode *>(pattern)) {
            const auto *actualFunc =
                dynamic_cast<const FuncPtrTypeNode *>(stripDecoratedTypeNode(
                    actualTypeNode));
            if (!actualFunc || actualFunc->args.size() != func->args.size()) {
                return;
            }
            for (std::size_t i = 0; i < func->args.size(); ++i) {
                inferGenericCapabilitySubsts(func->args[i], actualFunc->args[i],
                                             noGenericCapability(), substs);
            }
            inferGenericCapabilitySubsts(func->ret, actualFunc->ret,
                                         noGenericCapability(), substs);
            return;
        }
        if (auto *applied = dynamic_cast<const AppliedTypeNode *>(pattern)) {
            const auto *actualApplied =
                dynamic_cast<const AppliedTypeNode *>(stripDecoratedTypeNode(
                    actualTypeNode));
            auto *patternBase =
                dynamic_cast<const BaseTypeNode *>(applied->base);
            auto *actualBase = actualApplied
                                   ? dynamic_cast<const BaseTypeNode *>(
                                         actualApplied->base)
                                   : nullptr;
            if (!actualApplied || !sameVisibleTypeBase(patternBase, actualBase) ||
                actualApplied->args.size() != applied->args.size()) {
                return;
            }
            for (std::size_t i = 0; i < applied->args.size(); ++i) {
                inferGenericCapabilitySubsts(applied->args[i],
                                             actualApplied->args[i],
                                             noGenericCapability(), substs);
            }
        }
    }

    GenericCapabilityInfo inferGenericCallResultInfo(
        const AstFieldCall *node) const {
        auto *binding = resolvedCallTarget(node);
        if (binding &&
            binding->kind() == ResolvedEntityRef::Kind::GenericFunction) {
            auto *functionDecl = binding->functionDecl();
            if (!functionDecl || !functionDecl->returnTypeNode) {
                return noGenericCapability();
            }

            std::unordered_map<std::string, GenericCapabilityInfo> substs;
            auto *explicitTypeArgs = callExplicitTypeArgs(node);
            if (explicitTypeArgs) {
                const auto count = std::min(explicitTypeArgs->size(),
                                            functionDecl->typeParams.size());
                for (std::size_t i = 0; i < count; ++i) {
                    recordGenericCapabilitySubst(
                        substs,
                        toStringRef(functionDecl->typeParams[i].localName),
                        classifyGenericTypeNode(explicitTypeArgs->at(i)));
                }
            }

            const auto argCount = node->args ? node->args->size() : 0;
            const auto paramCount =
                std::min(argCount, functionDecl->paramTypeNodes.size());
            for (std::size_t i = 0; i < paramCount; ++i) {
                auto *argExpr = callArgValue(node->args->at(i));
                inferGenericCapabilitySubsts(functionDecl->paramTypeNodes[i],
                                             exprVisibleTypeNode(argExpr),
                                             inferGenericExprInfo(argExpr),
                                             substs);
            }

            return classifyGenericTypeNode(functionDecl->returnTypeNode, substs);
        }

        auto *callee = dynamic_cast<const AstDotLike *>(callTargetNode(node));
        if (!callee) {
            return noGenericCapability();
        }
        std::unordered_map<std::string, GenericCapabilityInfo> substs;
        auto fieldName = llvm::StringRef(callee->field->text.tochara(),
                                         callee->field->text.size());
        auto *methodDecl = resolveVisibleMethodDecl(
            projectionOwnerTypeNode(callee->parent), fieldName, &substs);
        if (!methodDecl || !methodDecl->retType) {
            auto *calleeTypeNode = exprVisibleTypeNode(callTargetNode(node));
            if (auto *func =
                    dynamic_cast<const FuncPtrTypeNode *>(
                        stripDecoratedTypeNode(calleeTypeNode))) {
                return classifyGenericTypeNode(func->ret);
            }
            if (auto *array =
                    dynamic_cast<const ArrayTypeNode *>(
                        stripDecoratedTypeNode(calleeTypeNode))) {
                return classifyGenericTypeNode(array->base);
            }
            if (auto *indexable =
                    dynamic_cast<const IndexablePointerTypeNode *>(
                        stripDecoratedTypeNode(calleeTypeNode))) {
                return classifyGenericTypeNode(indexable->base);
            }
            return noGenericCapability();
        }
        auto *explicitTypeArgs = callExplicitTypeArgs(node);
        if (methodDecl->typeParams) {
            if (explicitTypeArgs) {
                const auto count =
                    std::min(explicitTypeArgs->size(), methodDecl->typeParams->size());
                for (std::size_t i = 0; i < count; ++i) {
                    auto *param = methodDecl->typeParams->at(i);
                    if (!param) {
                        continue;
                    }
                    recordGenericCapabilitySubst(
                        substs, toStringRef(param->name.text),
                        classifyGenericTypeNode(explicitTypeArgs->at(i)));
                }
            }
            const auto argCount = node->args ? node->args->size() : 0;
            auto *args = node->args;
            const auto paramCount =
                std::min(argCount, methodDecl->args ? methodDecl->args->size() : 0);
            for (std::size_t i = 0; i < paramCount; ++i) {
                auto *argExpr = callArgValue(args->at(i));
                auto *argDecl = dynamic_cast<AstVarDecl *>(methodDecl->args->at(i));
                inferGenericCapabilitySubsts(
                    argDecl ? argDecl->typeNode : nullptr,
                    exprVisibleTypeNode(argExpr), inferGenericExprInfo(argExpr),
                    substs);
            }
        }
        return classifyGenericTypeNode(methodDecl->retType, substs);
    }

    GenericCapabilityInfo inferGenericExprInfo(const AstNode *node) const {
        if (!node) {
            return noGenericCapability();
        }
        if (auto *field = dynamic_cast<const AstField *>(node)) {
            auto *binding = resolved_.field(field);
            if (!binding ||
                binding->kind() != ResolvedEntityRef::Kind::LocalBinding) {
                return noGenericCapability();
            }
            return bindingGenericInfo(binding->localBinding());
        }
        if (auto *refExpr = dynamic_cast<const AstRefExpr *>(node)) {
            return inferGenericExprInfo(refExpr->expr);
        }
        if (auto *unary = dynamic_cast<const AstUnaryOper *>(node)) {
            auto info = inferGenericExprInfo(unary->expr);
            if (!info.valid()) {
                return info;
            }
            if (unary->op == '&') {
                ++info.pointerDepth;
                return info;
            }
            if (unary->op == '*') {
                if (info.pointerDepth > 0) {
                    --info.pointerDepth;
                    return info;
                }
                return noGenericCapability();
            }
            return noGenericCapability();
        }
        if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
            if (auto *binding = resolved_.dotLike(dotLike);
                binding && binding->valid()) {
                return noGenericCapability();
            }
            auto fieldName = llvm::StringRef(dotLike->field->text.tochara(),
                                             dotLike->field->text.size());
            if (auto *parentField = dynamic_cast<const AstField *>(dotLike->parent)) {
                if (auto *binding = resolved_.field(parentField);
                    binding &&
                    binding->kind() == ResolvedEntityRef::Kind::LocalBinding &&
                    binding->localBinding() == resolved_.selfBinding()) {
                    auto info = selfFieldGenericInfo(fieldName);
                    if (info.valid()) {
                        return info;
                    }
                }
            }
            auto *ownerTypeNode = projectionOwnerTypeNode(dotLike->parent);
            return projectedFieldGenericInfo(ownerTypeNode, fieldName);
        }
        if (auto *call = dynamic_cast<const AstFieldCall *>(node)) {
            return inferGenericCallResultInfo(call);
        }
        return noGenericCapability();
    }

    void rememberBindingGenericInfo(const ResolvedLocalBinding *binding,
                                    const TypeNode *declType,
                                    const AstNode *initExpr = nullptr) {
        if (!binding) {
            return;
        }
        auto info = classifyGenericTypeNode(declType);
        if (!info.valid() && initExpr) {
            info = inferGenericExprInfo(initExpr);
        }
        if (info.valid()) {
            bindingGenericInfo_[binding] = std::move(info);
        }
    }

    std::string genericCapabilityHint(const GenericCapabilityInfo &info) const {
        if (auto bound = genericTypeParamBoundName(info); !bound.empty()) {
            return "Bounded generic parameters only allow methods provided by bound `" +
                   bound +
                   "`, such as `value.method()`. Field access and operators on `T` stay unavailable even with a bound.";
        }
        return "Unconstrained generic parameters only allow type-level uses "
               "such as `sizeof[T]()`, `T*`, `T const*`, or `Box[T]`. "
               "Member access and operators require a future bound or a "
               "concrete type.";
    }

    const ModuleInterface::TraitDecl *resolveVisibleTraitDecl(
        const std::string &rawName,
        const ModuleInterface *ownerInterface = nullptr) const {
        if (rawName.empty()) {
            return nullptr;
        }

        auto lookupLocalTrait =
            [](const ModuleInterface *interface,
               const std::string &localName)
                -> const ModuleInterface::TraitDecl * {
            if (!interface) {
                return nullptr;
            }
            return interface->findTrait(localName);
        };

        auto dotPos = rawName.find('.');
        if (dotPos == std::string::npos) {
            if (auto *traitDecl = lookupLocalTrait(ownerInterface, rawName)) {
                return traitDecl;
            }
            if (!unit_ || !unit_->interface()) {
                return nullptr;
            }
            return unit_->interface()->findTrait(rawName);
        }

        auto moduleName = rawName.substr(0, dotPos);
        auto memberName = rawName.substr(dotPos + 1);
        if (ownerInterface) {
            if (moduleName == toStdString(ownerInterface->moduleName())) {
                if (auto *traitDecl =
                        lookupLocalTrait(ownerInterface, memberName)) {
                    return traitDecl;
                }
            }
            if (const auto *imported =
                    ownerInterface->findImportedModule(moduleName);
                imported && imported->interface) {
                if (auto *traitDecl =
                        lookupLocalTrait(imported->interface, memberName)) {
                    return traitDecl;
                }
            }
        }

        if (!unit_ || !unit_->interface()) {
            return nullptr;
        }
        if (moduleName == toStdString(unit_->moduleName())) {
            return unit_->interface()->findTrait(memberName);
        }
        const auto *imported = unit_->findImportedModule(moduleName);
        if (!imported || !imported->interface) {
            return nullptr;
        }
        return imported->interface->findTrait(memberName);
    }

    std::string methodParentTypeParamBoundName(
        const GenericCapabilityInfo &info) const {
        if (!resolved_.isMethod() || !unit_ || !unit_->interface()) {
            return {};
        }
        auto methodParentName = toStdString(resolved_.methodParentTypeName());
        const ModuleInterface::TypeDecl *typeDecl = nullptr;
        for (const auto &entry : unit_->interface()->types()) {
            const auto &candidate = entry.second;
            if (toStdString(candidate.localName) == methodParentName ||
                toStdString(candidate.exportedName) == methodParentName) {
                typeDecl = &candidate;
                break;
            }
        }
        if (!typeDecl) {
            if (auto dotPos = methodParentName.rfind('.');
                dotPos != std::string::npos) {
                auto localName = methodParentName.substr(dotPos + 1);
                typeDecl = unit_->interface()->findType(localName);
            } else {
                typeDecl = unit_->interface()->findType(methodParentName);
            }
        }
        if (!typeDecl) {
            return {};
        }
        for (const auto &param : typeDecl->typeParams) {
            if (toStdString(param.localName) == toStdString(info.paramName) &&
                !param.boundTraitName.empty()) {
                return toStdString(param.boundTraitName);
            }
        }
        return {};
    }

    std::string genericTypeParamBoundName(
        const GenericCapabilityInfo &info) const {
        if (const auto *bound =
                resolved_.genericTypeParamBound(toStdString(info.paramName))) {
            return *bound;
        }
        if (auto bound = methodParentTypeParamBoundName(info); !bound.empty()) {
            return bound;
        }
        if (const auto *boundSyntax =
                genericTypeParamBoundSyntax(toStringRef(info.paramName))) {
            return describeDotLikeSyntax(boundSyntax, "<trait>");
        }
        return {};
    }

    const ModuleInterface::TraitDecl *resolveBoundTraitDecl(
        const GenericCapabilityInfo &info) const {
        if (const auto *boundSyntax =
                genericTypeParamBoundSyntax(toStringRef(info.paramName))) {
            if (auto *traitDecl = resolveVisibleTraitDecl(
                    describeDotLikeSyntax(boundSyntax, "<trait>"),
                    resolved_.genericOwnerInterface())) {
                return traitDecl;
            }
        }
        auto bound = genericTypeParamBoundName(info);
        if (bound.empty()) {
            return nullptr;
        }
        return resolveVisibleTraitDecl(bound, resolved_.genericOwnerInterface());
    }

    [[noreturn]] void errorUnconstrainedGenericMemberUse(
        const location &loc, const GenericCapabilityInfo &info,
        llvm::StringRef memberName) const {
        if (auto bound = genericTypeParamBoundName(info); !bound.empty()) {
            error(loc,
                  "generic parameter `" + toStdString(info.paramName) +
                      "` does not provide member `" + memberName.str() +
                      "` through bound `" + bound + "`",
                  genericCapabilityHint(info));
        }
        error(loc,
              "unconstrained generic parameter `" + toStdString(info.paramName) +
                  "` does not provide member `" + memberName.str() + "`",
              genericCapabilityHint(info));
    }

    bool allowsBoundGenericMethodUse(const AstDotLike *dotLike,
                                     const GenericCapabilityInfo &info) const {
        if (!dotLike || !info.isDirectValue()) {
            return false;
        }
        const auto *traitDecl = resolveBoundTraitDecl(info);
        if (!traitDecl) {
            return false;
        }
        return traitDecl->findMethod(dotLike->field->text) != nullptr;
    }

    void resolveCalledSelector(const AstDotLike *dotLike) {
        if (!dotLike) {
            return;
        }
        resolveExpr(dotLike->parent);
        resolveDotLike(dotLike);
        auto info = inferGenericExprInfo(dotLike->parent);
        if (info.isDirectValue() &&
            !allowsBoundGenericMethodUse(dotLike, info)) {
            errorUnconstrainedGenericMemberUse(
                dotLike->loc, info,
                llvm::StringRef(dotLike->field->text.tochara(),
                                dotLike->field->text.size()));
        }
    }

    std::string describeOperator(token_type op) const {
        switch (op) {
            case Parser::token::LOGIC_EQUAL:
                return "==";
            case Parser::token::LOGIC_NOT_EQUAL:
                return "!=";
            case Parser::token::LOGIC_LE:
                return "<=";
            case Parser::token::LOGIC_GE:
                return ">=";
            case Parser::token::LOGIC_AND:
                return "&&";
            case Parser::token::LOGIC_OR:
                return "||";
            case Parser::token::SHIFT_LEFT:
                return "<<";
            case Parser::token::SHIFT_RIGHT:
                return ">>";
            default:
                return toStdString(symbolToStr(op));
        }
    }

    [[noreturn]] void errorUnconstrainedGenericOperatorUse(
        const location &loc, const GenericCapabilityInfo &info,
        token_type op) const {
        if (auto bound = genericTypeParamBoundName(info); !bound.empty()) {
            error(loc,
                  "generic parameter `" + toStdString(info.paramName) +
                      "` does not support operator `" + describeOperator(op) +
                      "` through bound `" + bound + "`",
                  genericCapabilityHint(info));
        }
        error(loc,
              "unconstrained generic parameter `" + toStdString(info.paramName) +
                  "` does not support operator `" + describeOperator(op) + "`",
              genericCapabilityHint(info));
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
                          "` applies `[...]` arguments to a non-generic type",
                      "Remove the `[...]` arguments, or make the base type "
                      "generic before specializing it.");
            }
            if (applied->args.size() != typeDecl->typeParams.size()) {
                error(applied->loc,
                      "generic type argument count mismatch for `" +
                          toStdString(typeDecl->exportedName) + "`: expected " +
                          std::to_string(typeDecl->typeParams.size()) +
                          ", got " +
                          std::to_string(applied->args.size()),
                      "Match the number of `[` `]` type arguments to the "
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
            rememberBindingGenericInfo(binding, varDef->getTypeNode(),
                                       varDef->withInitVal()
                                           ? varDef->getInitVal()
                                           : nullptr);
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
            auto leftInfo = inferGenericExprInfo(bin->left);
            if (leftInfo.isDirectValue()) {
                errorUnconstrainedGenericOperatorUse(bin->loc, leftInfo,
                                                     bin->op);
            }
            auto rightInfo = inferGenericExprInfo(bin->right);
            if (rightInfo.isDirectValue()) {
                errorUnconstrainedGenericOperatorUse(bin->loc, rightInfo,
                                                     bin->op);
            }
            return;
        }
        if (auto *unary = dynamic_cast<const AstUnaryOper *>(node)) {
            resolveExpr(unary->expr);
            if (unary->op != '&' && unary->op != '*') {
                auto info = inferGenericExprInfo(unary->expr);
                if (info.isDirectValue()) {
                    errorUnconstrainedGenericOperatorUse(unary->loc, info,
                                                         unary->op);
                }
            }
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
            auto info = inferGenericExprInfo(dotLike->parent);
            if (info.isDirectValue()) {
                errorUnconstrainedGenericMemberUse(
                    dotLike->loc, info,
                    llvm::StringRef(dotLike->field->text.tochara(),
                                    dotLike->field->text.size()));
            }
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
            if (auto *typeApply = dynamic_cast<const AstTypeApply *>(call->value)) {
                if (auto *dotLike =
                        dynamic_cast<const AstDotLike *>(typeApply->value)) {
                    resolveCalledSelector(dotLike);
                } else {
                    resolveExpr(call->value);
                }
            } else if (auto *dotLike =
                           dynamic_cast<const AstDotLike *>(call->value)) {
                resolveCalledSelector(dotLike);
            } else {
                resolveExpr(call->value);
            }
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
            rememberBindingGenericInfo(binding, bindingDeclaredTypeNode(binding));
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
        std::unordered_map<std::string, std::string> genericTypeParamBounds = {},
        const ModuleInterface *genericOwnerInterface = nullptr,
        std::unordered_map<std::string, TypeClass *> concreteGenericTypes = {}) {
        auto *resolved = module_->createFunction(
            decl, body, std::move(functionName),
            std::move(methodParentTypeName), loc, topLevelEntry, languageEntry,
            guaranteedReturn, templateValidationOnly,
            std::move(genericTypeParams), std::move(genericTypeParamBounds),
            genericOwnerInterface,
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

    void resolveFunction(
        AstFuncDecl *node, StructType *methodParent = nullptr,
        string methodParentTypeName = string(),
        std::vector<string> scopedTypeParams = {},
        std::unordered_map<std::string, std::string> scopedTypeParamBounds = {}) {
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

        auto genericTypeParamBounds = std::move(scopedTypeParamBounds);
        auto ownGenericTypeParamBounds = collectGenericParamBounds(node->typeParams);
        genericTypeParamBounds.insert(ownGenericTypeParamBounds.begin(),
                                      ownGenericTypeParamBounds.end());
        auto *resolved = createResolvedFunction(
            node, node->body,
            methodParent ? string(node->name)
                         : (templateValidationOnly ? string()
                                                   : resolvedFunctionName),
            !methodParentTypeName.empty()
                ? std::move(methodParentTypeName)
                : (methodParent ? string(methodParent->full_name) : string()),
            node->loc, false, false, node->body && node->body->hasTerminator(),
            templateValidationOnly, std::move(scopedTypeParams),
            std::move(genericTypeParamBounds));
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
        auto scopedTypeParamBounds = collectGenericParamBounds(node->typeParams);
        for (auto *stmt : body->getBody()) {
            auto *func = dynamic_cast<AstFuncDecl *>(stmt);
            if (func) {
                resolveFunction(func, structType, resolvedStructName,
                                scopedTypeParams, scopedTypeParamBounds);
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
                                   std::unordered_map<std::string, std::string>
                                       genericTypeParamBounds,
                                   const ModuleInterface *genericOwnerInterface,
                                   std::unordered_map<std::string, TypeClass *>
                                       concreteGenericTypes) {
    functions_.push_back(std::make_unique<ResolvedFunction>(
        decl, body, std::move(functionName), std::move(methodParentTypeName),
        loc, topLevelEntry, languageEntry, guaranteedReturn,
        templateValidationOnly, std::move(genericTypeParams),
        std::move(genericTypeParamBounds),
        genericOwnerInterface,
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
    const ModuleInterface *genericOwnerInterface,
    std::unordered_map<std::string, TypeClass *> concreteGenericTypes) {
    if (!global || !decl) {
        return nullptr;
    }

    auto *typeMgr = global->types();
    assert(typeMgr);

    std::vector<string> genericTypeParams;
    std::unordered_map<std::string, std::string> genericTypeParamBounds;
    if (decl->typeParams) {
        genericTypeParams.reserve(decl->typeParams->size());
        for (auto *token : *decl->typeParams) {
            if (token) {
                genericTypeParams.push_back(token->name.text);
                if (token->hasBoundTrait()) {
                    genericTypeParamBounds.emplace(
                        toStdString(token->name.text),
                        describeDotLikeSyntax(token->boundTrait, "<trait>"));
                }
            }
        }
    }

    auto module = std::make_unique<ResolvedModule>();
    auto *resolved = module->createFunction(
        decl, decl->body, std::move(resolvedFunctionName), string(), decl->loc,
        false, false, decl->body && decl->body->hasTerminator(), false,
        std::move(genericTypeParams), std::move(genericTypeParamBounds),
        genericOwnerInterface,
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
    string resolvedFunctionName, string methodParentTypeName,
    std::vector<string> genericTypeParams,
    std::unordered_map<std::string, std::string> genericTypeParamBounds,
    const ModuleInterface *genericOwnerInterface,
    std::unordered_map<std::string, TypeClass *> concreteGenericTypes) {
    if (!global || !decl || resolvedFunctionName.empty() ||
        methodParentTypeName.empty()) {
        return nullptr;
    }

    auto *typeMgr = global->types();
    assert(typeMgr);

    auto module = std::make_unique<ResolvedModule>();
    auto *resolved = module->createFunction(
        decl, decl->body, std::move(resolvedFunctionName),
        std::move(methodParentTypeName), decl->loc, false, false,
        decl->body && decl->body->hasTerminator(), false,
        std::move(genericTypeParams), std::move(genericTypeParamBounds),
        genericOwnerInterface,
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
