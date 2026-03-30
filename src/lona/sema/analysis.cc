#include "lona/abi/abi.hh"
#include "lona/ast/array_dim.hh"
#include "lona/ast/astnode.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/call_target_tools.hh"
#include "lona/sema/hir.hh"
#include "lona/sema/initializer_semantics.hh"
#include "lona/sema/injected_member.hh"
#include "lona/sema/operator_resolver.hh"
#include "lona/sym/func.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include "parser.hh"
#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lona {
namespace analysis_impl {

std::string
describeResolvedFuncType(FuncType *type, size_t argOffset = 0) {
    if (type == nullptr) {
        return "<unknown type>";
    }

    std::string name = "(";
    const auto &argTypes = type->getArgTypes();
    for (size_t i = argOffset; i < argTypes.size(); ++i) {
        if (i != argOffset) {
            name += ", ";
        }
        if (type->getArgBindingKind(i) == BindingKind::Ref) {
            name += "ref ";
        }
        name += describeResolvedType(argTypes[i]);
    }
    name += ":";
    if (type->getRetType() != nullptr) {
        name += " ";
        name += describeResolvedType(type->getRetType());
    }
    name += ")";
    return name;
}

std::string
describeStorageType(TypeClass *type, AstVarDef *node) {
    if (node != nullptr && node->getTypeNode() != nullptr) {
        return describeTypeNode(node->getTypeNode());
    }
    return describeResolvedType(type);
}

std::string
describeMemberOwnerSyntax(const AstNode *node) {
    if (!node) {
        return "";
    }
    if (node->is<AstField>() || node->is<AstDotLike>()) {
        return describeDotLikeSyntax(node);
    }
    return "";
}

std::string
describeTupleFieldHelp(TupleType *tupleType) {
    if (!tupleType || tupleType->getItemTypes().empty()) {
        return "Tuple fields are named `_1`, `_2`, ... in declaration order.";
    }

    std::string hint = "Tuple fields are named ";
    const auto &itemTypes = tupleType->getItemTypes();
    for (size_t i = 0; i < itemTypes.size(); ++i) {
        if (i != 0) {
            hint += ", ";
        }
        hint += "`";
        hint += TupleType::buildFieldName(i);
        hint += "`";
    }
    hint += " in declaration order.";
    return hint;
}

std::string
castDomainHint() {
    return "Builtin cast only supports builtin scalar and pointer types. "
           "Convert structs and tuples field-by-field, or use `.tobits()` when "
           "you need raw bit-copy behavior.";
}

bool
isBuiltinCastType(TypeClass *type) {
    return isByteCopyPlainType(type);
}

bool
isDirectFunctionPointerValueType(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    auto *pointerType = storageType ? storageType->as<PointerType>() : nullptr;
    return pointerType && pointerType->getPointeeType() &&
           pointerType->getPointeeType()->as<FuncType>();
}

bool
isFixedArrayOfFunctionPointerValues(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    auto *arrayType = storageType ? storageType->as<ArrayType>() : nullptr;
    if (!arrayType) {
        return false;
    }
    auto *elementType = arrayType->getElementType();
    return isDirectFunctionPointerValueType(elementType) ||
           isFixedArrayOfFunctionPointerValues(elementType);
}

std::string
describeInjectedMemberHelp(TypeClass *receiverType,
                           const std::string &memberName) {
    (void)receiverType;
    return "Call injected members directly as `<expr>." + memberName +
           "(...)`. Raw bit-copy helpers are injected as `value.tobits()` and "
           "`u8[N].toXXX()`.";
}

FuncType *
getMethodSelectorType(TypeTable *typeMgr, HIRSelector *selector) {
    if (!selector || !selector->isMethodSelector()) {
        return nullptr;
    }

    auto *parentType =
        selector->getParent() ? selector->getParent()->getType() : nullptr;
    auto *structType = asUnqualified<StructType>(parentType);
    if (!structType) {
        return nullptr;
    }

    auto *func = typeMgr
                     ? typeMgr->getMethodFunction(
                           structType, toStringRef(selector->getFieldName()))
                     : nullptr;
    return func && func->getType() ? func->getType()->as<FuncType>() : nullptr;
}

size_t
getMethodCallArgOffset(HIRSelector *selector, FuncType *type) {
    if (!selector || !type) {
        return 0;
    }

    auto *parentType =
        selector->getParent() ? selector->getParent()->getType() : nullptr;
    const auto &argTypes = type->getArgTypes();
    auto *selfPointeeType = !argTypes.empty()
                                ? getRawPointerPointeeType(argTypes.front())
                                : nullptr;
    if (selfPointeeType && parentType &&
        asUnqualified<StructType>(selfPointeeType) ==
            asUnqualified<StructType>(parentType)) {
        return 1;
    }
    return 0;
}

std::string
describeMethodSelectorType(HIRSelector *selector, FuncType *type) {
    if (!selector || !type) {
        return "<unknown type>";
    }

    return describeResolvedFuncType(type,
                                    getMethodCallArgOffset(selector, type));
}

void
rejectMethodSelectorStorage(TypeTable *typeMgr, HIRExpr *expr,
                            AstVarDef *node) {
    auto *selector = dynamic_cast<HIRSelector *>(expr);
    auto *funcType = getMethodSelectorType(typeMgr, selector);
    if (!selector || !funcType || !node) {
        return;
    }

    error(node->loc,
          "unsupported bare function variable type for `" +
              toStdString(node->getName()) +
              "`: " + describeMethodSelectorType(selector, funcType),
          "Store an explicit function pointer instead of a bare method "
          "selector.");
}

void
rejectNonCallMethodSelector(TypeTable *typeMgr, HIRExpr *expr) {
    auto *selector = dynamic_cast<HIRSelector *>(expr);
    if (!selector || !getMethodSelectorType(typeMgr, selector)) {
        return;
    }

    error(selector->getLocation(), kMethodSelectorDirectCallError,
          "Call the method directly as `obj.method(...)`.");
}

std::optional<InjectedMemberBinding>
resolveInjectedMemberBinding(TypeTable *typeMgr, TypeClass *receiverType,
                             const std::string &memberName) {
    if (!typeMgr || !receiverType) {
        return std::nullopt;
    }
    return resolveInjectedMember(typeMgr, receiverType,
                                 llvm::StringRef(memberName));
}

std::optional<InjectedMemberBinding>
resolveInjectedMemberBinding(TypeTable *typeMgr, HIRExpr *receiver,
                             const std::string &memberName) {
    return resolveInjectedMemberBinding(
        typeMgr, receiver ? receiver->getType() : nullptr, memberName);
}

void
rejectBareFunctionStorage(TypeClass *type, AstVarDef *node) {
    if (!node) {
        return;
    }
    bool hasBareFunctionStorage = type && type->as<FuncType>();
    if (!hasBareFunctionStorage) {
        return;
    }
    error(
        node->loc,
        "unsupported bare function variable type for `" +
            toStdString(node->getName()) +
            "`: " + describeStorageType(type, node),
        "Use an explicit function pointer type like `(T1, T2: Ret)` instead.");
}

void
rejectOpaqueStructStorage(TypeClass *type, AstVarDef *node) {
    if (!node || !type) {
        return;
    }
    auto *structType = asUnqualified<StructType>(type);
    if (!structType || !structType->isOpaque()) {
        return;
    }
    error(node->loc,
          "opaque struct `" + describeStorageType(type, node) +
              "` cannot be used by value in variable `" +
              toStdString(node->getName()) + "`",
          "Use `" + describeStorageType(type, node) +
              "*` instead. Opaque structs are only supported behind pointers.");
}

void
rejectConstVariableStorage(TypeClass *type, AstVarDef *node) {
    if (!node || !type || node->isRefBinding() || node->isReadOnlyBinding()) {
        return;
    }
    auto *typeNode = node->getTypeNode();
    if (!typeNode || dynamic_cast<ConstTypeNode *>(typeNode) == nullptr) {
        return;
    }
    error(node->loc,
          "variable `" + toStdString(node->getName()) +
              "` cannot use a top-level const storage type: " +
              describeStorageType(type, node),
          "Use `const " + toStdString(node->getName()) + " = ...` or `const " +
              toStdString(node->getName()) +
              " T = ...` for a read-only binding, or move `const` behind a "
              "pointer like `T const*` / `T const[*]` when you only want a "
              "read-only pointee view.");
}

void
rejectUninitializedFunctionPointerValueStorage(TypeClass *type,
                                               AstVarDef *node) {
    if (!node || node->withInitVal() || !type) {
        return;
    }

    if (isDirectFunctionPointerValueType(type)) {
        error(node->loc,
              "function pointer variable type for `" +
                  toStdString(node->getName()) +
                  "` requires initializer: " + describeStorageType(type, node),
              "Initialize function pointers at the point of definition.");
        return;
    }

    if (!isFixedArrayOfFunctionPointerValues(type)) {
        return;
    }

    error(
        node->loc,
        "function pointer array variable for `" + toStdString(node->getName()) +
            "` requires a full initializer: " + describeStorageType(type, node),
        "Initialize every slot explicitly. Missing elements would become null "
        "function pointers.");
}

TypeTable *
requireTypeTable(Scope *scope) {
    assert(scope);
    auto *typeMgr = scope->types();
    assert(typeMgr);
    return typeMgr;
}

llvm::StringRef
languageEntrySymbolName() {
    return "__lona_main__";
}

FuncType *
getOrCreateMainType(TypeTable *typeMgr) {
    return typeMgr->getOrCreateFunctionType({}, i32Ty);
}

llvm::Function *
getOrCreateTopLevelEntry(GlobalScope *global, TypeTable *typeMgr) {
    auto *mainType = getOrCreateMainType(typeMgr);
    auto entryName = languageEntrySymbolName();
    if (auto *existing = global->module.getFunction(entryName)) {
        return existing;
    }

    auto *entry = llvm::Function::Create(
        typeMgr->getLLVMFunctionType(mainType), llvm::Function::ExternalLinkage,
        llvm::Twine(entryName), global->module);
    annotateFunctionAbi(*entry, AbiKind::Native);
    return entry;
}

class FunctionAnalyzer {
    TypeTable *typeMgr;
    GlobalScope *global;
    const CompilationUnit *unit;
    const ResolvedFunction &resolved;
    OperatorResolver operatorResolver;
    HIRModule *ownerModule;
    HIRFunc *hirFunc;
    std::unordered_map<const ResolvedLocalBinding *, ObjectPtr> bindingObjects;
    int loopDepth = 0;

    [[noreturn]] void error(const location &loc, const std::string &message,
                            const std::string &hint = std::string()) {
        lona::error(loc, message, hint);
    }

    [[noreturn]] void internalError(const location &loc,
                                    const std::string &message,
                                    const std::string &hint = std::string()) {
        throw DiagnosticError(DiagnosticError::Category::Internal, loc, message,
                              hint);
    }

    TypeClass *requireType(TypeNode *node, const location &loc,
                           const std::string &context) {
        validateTypeNodeLayout(node);
        if (isReservedInitialListTypeNode(node)) {
            errorReservedInitialListType(node->loc);
        }
        auto *type =
            unit ? unit->resolveType(typeMgr, node) : typeMgr->getType(node);
        if (!type) {
            error(loc, context);
        }
        return type;
    }

    void requireCompatibleTypes(const location &loc, TypeClass *expectedType,
                                TypeClass *actualType,
                                const std::string &context) {
        requireCompatibleInitializerTypes(loc, expectedType, actualType,
                                          context);
    }

    bool canBindReferenceType(TypeClass *targetType, TypeClass *sourceType) {
        return targetType && sourceType &&
               isConstQualificationConvertible(targetType, sourceType);
    }

    [[noreturn]] void errorReadOnlyAssignmentTarget(const location &loc,
                                                    TypeClass *type) {
        error(loc,
              "assignment target contains read-only storage: " +
                  describeResolvedType(type),
              "Only fully writable values can appear on the left side of `=`. "
              "Write through a mutable projection instead, or copy into a new "
              "`var` binding if you need a writable whole value.");
    }

    template<typename T, typename... Args>
    T *makeHIR(Args &&...args) {
        assert(ownerModule);
        return ownerModule->create<T>(std::forward<Args>(args)...);
    }

    void bindObject(const ResolvedLocalBinding *binding, ObjectPtr object) {
        assert(binding);
        assert(object);
        bindingObjects[binding] = object;
    }

    AstNode *makeStaticDimensionNode(std::size_t extent, const location &loc) {
        auto text = std::to_string(extent);
        AstToken token(TokenType::ConstInt32, text.c_str(), loc);
        return new AstConst(token);
    }

    ObjectPtr requireBoundObject(const ResolvedLocalBinding *binding,
                                 const location &loc) {
        if (!binding) {
            internalError(loc, "missing resolved local binding",
                          "Run name resolution before HIR lowering.");
        }
        auto found = bindingObjects.find(binding);
        if (found == bindingObjects.end()) {
            internalError(loc,
                          "resolved local binding `" +
                              toStdString(binding->name()) +
                              "` was not materialized before use",
                          "This looks like a compiler pipeline bug.");
        }
        return found->second;
    }

    ObjectPtr requireGlobalObject(const ::string &name, const location &loc,
                                  const std::string &context) {
        auto *obj = global->getObj(name);
        if (!obj) {
            internalError(
                loc,
                "resolved " + context + " `" + toStdString(name) +
                    "` is missing from the current global scope",
                "Rebuild declarations before reusing this resolved module.");
        }
        return obj;
    }

    Function *requireGlobalFunction(const ::string &name, const location &loc,
                                    const std::string &context) {
        auto *obj = requireGlobalObject(name, loc, context);
        auto *func = obj->as<Function>();
        if (!func) {
            internalError(
                loc,
                "resolved " + context + " `" + toStdString(name) +
                    "` no longer refers to a function",
                "Rebuild declarations before reusing this resolved module.");
        }
        return func;
    }

    StructType *requireStructTypeByName(const ::string &name,
                                        const location &loc,
                                        const std::string &context) {
        auto *type = typeMgr->getType(name);
        auto *structType = type ? type->as<StructType>() : nullptr;
        if (!structType) {
            internalError(
                loc,
                "resolved " + context + " `" + toStdString(name) +
                    "` is missing from the current type table",
                "Rebuild declarations before reusing this resolved module.");
        }
        return structType;
    }

    TypeClass *requireTypeByName(const ::string &name, const location &loc,
                                 const std::string &context) {
        auto *type = typeMgr->getType(name);
        if (!type) {
            internalError(
                loc,
                "resolved " + context + " `" + toStdString(name) +
                    "` is missing from the current type table",
                "Rebuild declarations before reusing this resolved module.");
        }
        return type;
    }

    StructType *currentMethodParentType() {
        if (!resolved.isMethod()) {
            return nullptr;
        }
        return requireStructTypeByName(resolved.methodParentTypeName(),
                                       resolved.loc(), "method parent type");
    }

    bool hasInternalFieldAccess(StructType *ownerType) {
        auto *methodParent = currentMethodParentType();
        return ownerType && methodParent == ownerType;
    }

    TypeClass *applySlotConst(TypeClass *type) {
        if (!type) {
            return nullptr;
        }
        if (isConstQualifiedType(type)) {
            return type;
        }
        return typeMgr->createConstType(type);
    }

    TypeClass *projectStructFieldType(StructType *ownerStructType,
                                      TypeClass *ownerValueType,
                                      llvm::StringRef fieldName,
                                      TypeClass *fieldType) {
        if (!fieldType) {
            return nullptr;
        }
        if (!ownerStructType) {
            return fieldType;
        }

        bool requiresConstView = isConstQualifiedType(ownerValueType);
        if (!requiresConstView &&
            ownerStructType->getMemberAccess(fieldName) ==
                AccessKind::GetOnly &&
            !hasInternalFieldAccess(ownerStructType)) {
            requiresConstView = true;
        }
        return requiresConstView ? applySlotConst(fieldType) : fieldType;
    }

    TypeClass *projectArrayElementType(TypeClass *containerType,
                                       TypeClass *elementType) {
        if (!elementType) {
            return nullptr;
        }
        return isConstQualifiedType(containerType) ? applySlotConst(elementType)
                                                   : elementType;
    }

    TypeClass *projectTupleMemberType(TypeClass *tupleType,
                                      TypeClass *memberType) {
        if (!memberType) {
            return nullptr;
        }
        return isConstQualifiedType(tupleType) ? applySlotConst(memberType)
                                               : memberType;
    }

    TypeClass *getMethodReceiverPointee(FuncType *funcType) {
        if (!funcType || funcType->getArgTypes().empty()) {
            return nullptr;
        }
        return getRawPointerPointeeType(funcType->getArgTypes().front());
    }

    void requireMethodReceiverCompatible(HIRSelector *selector,
                                         FuncType *funcType,
                                         const location &loc) {
        if (!selector || !funcType) {
            return;
        }

        auto *parentType =
            selector->getParent() ? selector->getParent()->getType() : nullptr;
        auto *receiverPointeeType = getMethodReceiverPointee(funcType);
        if (!parentType || !receiverPointeeType) {
            internalError(
                loc, "method call is missing its receiver type information",
                "This looks like a compiler pipeline bug.");
        }
        if (isConstQualificationConvertible(receiverPointeeType, parentType)) {
            return;
        }

        error(loc,
              "set method `" + toStdString(selector->getFieldName()) +
                  "` requires a writable receiver, got " +
                  describeResolvedType(parentType),
              "Call it on a writable value, or use a non-`set` method here.");
    }

    EntityRef classifyEntity(HIRExpr *expr) {
        if (!expr) {
            return EntityRef::invalid();
        }
        if (auto *valueExpr = dynamic_cast<HIRValue *>(expr)) {
            auto *value = valueExpr->getValue();
            if (!value) {
                return EntityRef::invalid();
            }
            if (auto *typeObject = value->as<TypeObject>()) {
                return EntityRef::type(typeObject->declaredType());
            }
            return EntityRef::object(value);
        }
        if (auto *type = expr->getType()) {
            return EntityRef::typedValue(type);
        }
        return EntityRef::invalid();
    }

    struct MemberLookupOwner {
        EntityRef entity;
        TypeClass *valueType = nullptr;
        TupleType *tupleType = nullptr;
        StructType *structType = nullptr;
    };

    struct MemberLookup {
        MemberLookupOwner owner;
        LookupResult result;
        std::optional<InjectedMemberBinding> injectedMember;
        std::vector<std::string> promotedPath;
        std::vector<std::vector<std::string>> ambiguousPromotedPaths;
    };

    struct MemberLookupAttempt {
        HIRExpr *parent = nullptr;
        MemberLookup lookup;
    };

    struct CallResolutionAttempt {
        HIRExpr *callee = nullptr;
        CallResolution resolution;
    };

    static bool isExplicitDerefSyntax(const AstNode *node) {
        auto *unary = dynamic_cast<const AstUnaryOper *>(node);
        return unary && unary->op == '*';
    }

    HIRExpr *implicitDeref(HIRExpr *expr, const location &loc) {
        if (!expr || !asUnqualified<PointerType>(expr->getType())) {
            return nullptr;
        }
        auto binding = operatorResolver.resolveUnary('*', expr->getType(),
                                                     isAddressable(expr), loc);
        return makeHIR<HIRUnaryOper>(binding, expr, binding.resultType, loc);
    }

    MemberLookupOwner classifyMemberOwner(HIRExpr *parent) {
        MemberLookupOwner owner;
        owner.entity = classifyEntity(parent);
        owner.valueType = owner.entity.valueType();
        owner.tupleType = asUnqualified<TupleType>(owner.valueType);
        owner.structType = asUnqualified<StructType>(owner.valueType);
        return owner;
    }

    LookupResult lookupDirectValueMember(const MemberLookupOwner &owner,
                                         const std::string &fieldName) {
        auto result = owner.entity.dot(fieldName);
        if (owner.tupleType) {
            TupleType::ValueTy member;
            if (owner.tupleType->getMember(llvm::StringRef(fieldName),
                                           member)) {
                result.kind = LookupResultKind::ValueField;
                result.resultEntity = EntityRef::typedValue(
                    projectTupleMemberType(owner.valueType, member.first));
                return result;
            }
            result.kind = LookupResultKind::NotFound;
            return result;
        }

        if (owner.structType) {
            if (auto *member =
                    owner.structType->getMember(llvm::StringRef(fieldName))) {
                result.kind = LookupResultKind::ValueField;
                result.resultEntity =
                    EntityRef::typedValue(projectStructFieldType(
                        owner.structType, owner.valueType,
                        llvm::StringRef(fieldName), member->first));
                return result;
            }
            if (auto *methodType = owner.structType->getMethodType(
                    llvm::StringRef(fieldName))) {
                result.kind = LookupResultKind::Method;
                result.resultEntity = EntityRef::typedValue(methodType);
                return result;
            }
            result.kind = LookupResultKind::NotFound;
            return result;
        }

        result.kind = LookupResultKind::NotFound;
        return result;
    }

    struct PromotedLookupState {
        StructType *structType = nullptr;
        TypeClass *valueType = nullptr;
        std::vector<std::string> path;
    };

    std::string formatPromotedMemberPath(const std::vector<std::string> &path,
                                         const std::string &fieldName) {
        std::string name;
        for (const auto &segment : path) {
            if (!name.empty()) {
                name += ".";
            }
            name += segment;
        }
        if (!name.empty()) {
            name += ".";
        }
        name += fieldName;
        return name;
    }

    void collectEmbeddedLookupStates(const MemberLookupOwner &owner,
                                     std::vector<PromotedLookupState> &states) {
        if (!owner.structType) {
            return;
        }
        for (const auto &member : owner.structType->getMembers()) {
            if (!owner.structType->isEmbeddedMember(member.first())) {
                continue;
            }
            auto *embeddedValueType =
                projectStructFieldType(owner.structType, owner.valueType,
                                       member.first(), member.second.first);
            auto *embeddedStructType =
                asUnqualified<StructType>(embeddedValueType);
            if (!embeddedStructType) {
                continue;
            }
            states.push_back({embeddedStructType,
                              embeddedValueType,
                              {member.first().str()}});
        }
    }

    void collectEmbeddedLookupStates(const PromotedLookupState &owner,
                                     std::vector<PromotedLookupState> &states) {
        if (!owner.structType) {
            return;
        }
        for (const auto &member : owner.structType->getMembers()) {
            if (!owner.structType->isEmbeddedMember(member.first())) {
                continue;
            }
            auto *embeddedValueType =
                projectStructFieldType(owner.structType, owner.valueType,
                                       member.first(), member.second.first);
            auto *embeddedStructType =
                asUnqualified<StructType>(embeddedValueType);
            if (!embeddedStructType) {
                continue;
            }
            auto path = owner.path;
            path.push_back(member.first().str());
            states.push_back(
                {embeddedStructType, embeddedValueType, std::move(path)});
        }
    }

    LookupResult lookupPromotedValueMember(
        const MemberLookupOwner &owner, const std::string &fieldName,
        std::vector<std::string> &promotedPath,
        std::vector<std::vector<std::string>> &ambiguousPaths) {
        LookupResult result = owner.entity.dot(fieldName);
        result.kind = LookupResultKind::NotFound;
        promotedPath.clear();
        ambiguousPaths.clear();
        if (!owner.structType) {
            return result;
        }

        std::vector<PromotedLookupState> frontier;
        collectEmbeddedLookupStates(owner, frontier);
        while (!frontier.empty()) {
            struct Candidate {
                LookupResult result;
                std::vector<std::string> path;
            };

            std::vector<Candidate> candidates;
            std::vector<PromotedLookupState> next;
            for (const auto &state : frontier) {
                MemberLookupOwner promotedOwner;
                promotedOwner.entity = EntityRef::typedValue(state.valueType);
                promotedOwner.valueType = state.valueType;
                promotedOwner.structType = state.structType;
                auto promotedLookup =
                    lookupDirectValueMember(promotedOwner, fieldName);
                if (promotedLookup.kind == LookupResultKind::ValueField ||
                    promotedLookup.kind == LookupResultKind::Method) {
                    candidates.push_back({promotedLookup, state.path});
                }
                collectEmbeddedLookupStates(state, next);
            }

            if (candidates.size() == 1) {
                promotedPath = candidates.front().path;
                return candidates.front().result;
            }
            if (candidates.size() > 1) {
                result.kind = LookupResultKind::Ambiguous;
                for (const auto &candidate : candidates) {
                    ambiguousPaths.push_back(candidate.path);
                }
                return result;
            }
            frontier = std::move(next);
        }

        return result;
    }

    MemberLookup lookupMember(HIRExpr *parent, const std::string &fieldName,
                              const location &loc) {
        MemberLookup lookup;
        lookup.owner = classifyMemberOwner(parent);
        (void)loc;

        if (auto binding = resolveInjectedMemberBinding(
                typeMgr, lookup.owner.valueType, fieldName)) {
            lookup.result = lookup.owner.entity.dot(fieldName);
            lookup.result.kind = LookupResultKind::InjectedMember;
            lookup.result.resultEntity =
                EntityRef::typedValue(binding->resultType);
            lookup.injectedMember = binding;
            return lookup;
        }

        lookup.result = lookupDirectValueMember(lookup.owner, fieldName);
        if (lookup.result.kind != LookupResultKind::NotFound) {
            return lookup;
        }

        lookup.result = lookupPromotedValueMember(
            lookup.owner, fieldName, lookup.promotedPath,
            lookup.ambiguousPromotedPaths);
        return lookup;
    }

    MemberLookupAttempt lookupMemberWithImplicitDeref(
        HIRExpr *parent, const std::string &fieldName, const location &loc,
        bool allowImplicitDeref) {
        MemberLookupAttempt attempt;
        attempt.parent = parent;
        attempt.lookup = lookupMember(parent, fieldName, loc);
        if (!allowImplicitDeref ||
            attempt.lookup.result.kind != LookupResultKind::NotFound) {
            return attempt;
        }

        auto *derefParent = implicitDeref(parent, loc);
        if (!derefParent) {
            return attempt;
        }

        attempt.parent = derefParent;
        attempt.lookup = lookupMember(derefParent, fieldName, loc);
        return attempt;
    }

    HIRExpr *materializeMemberExpr(HIRExpr *parent,
                                   const std::string &fieldName,
                                   const MemberLookup &lookup,
                                   const location &loc,
                                   bool allowInjectedMember = false) {
        auto *current = parent;
        for (const auto &segment : lookup.promotedPath) {
            auto segmentOwner = classifyMemberOwner(current);
            auto segmentLookup = lookupDirectValueMember(segmentOwner, segment);
            if (segmentLookup.kind != LookupResultKind::ValueField) {
                internalError(
                    loc,
                    "promoted member path `" + segment +
                        "` did not resolve to a concrete field",
                    "This looks like a promoted-member lowering bug.");
            }
            current = makeHIR<HIRSelector>(
                current, segment, segmentLookup.resultEntity.valueType(), loc);
        }

        switch (lookup.result.kind) {
            case LookupResultKind::ValueField:
                return makeHIR<HIRSelector>(
                    current, fieldName, lookup.result.resultEntity.valueType(),
                    loc);
            case LookupResultKind::Method:
                return makeHIR<HIRSelector>(current, fieldName, nullptr, loc,
                                            HIRSelectorKind::Method);
            case LookupResultKind::InjectedMember:
                if (allowInjectedMember) {
                    return nullptr;
                }
                error(
                    loc,
                    "injected member `" + fieldName +
                        "` can only be used as a direct call callee",
                    describeInjectedMemberHelp(current->getType(), fieldName));
            default:
                return nullptr;
        }
    }

    [[noreturn]] void diagnoseMemberLookupFailure(
        const MemberLookup &lookup, const std::string &fieldName,
        const location &loc, const std::string &ownerLabel = std::string()) {
        if (lookup.owner.entity.asType()) {
            auto typeName =
                ownerLabel.empty()
                    ? describeResolvedType(lookup.owner.entity.asType())
                    : ownerLabel;
            error(loc,
                  "unknown type member `" + typeName + "." + fieldName + "`",
                  "Static type members are not implemented yet.");
        }

        if (lookup.owner.tupleType) {
            error(loc, "unknown tuple field `" + fieldName + "`",
                  describeTupleFieldHelp(lookup.owner.tupleType));
        }

        if (lookup.result.kind == LookupResultKind::Ambiguous) {
            std::vector<std::string> suggestions;
            suggestions.reserve(lookup.ambiguousPromotedPaths.size());
            for (const auto &path : lookup.ambiguousPromotedPaths) {
                auto suggestion =
                    ownerLabel.empty()
                        ? formatPromotedMemberPath(path, fieldName)
                        : ownerLabel + "." +
                              formatPromotedMemberPath(path, fieldName);
                suggestions.push_back("`" + suggestion + "`");
            }
            std::string help = "Use an explicit embedded path such as ";
            for (std::size_t i = 0; i < suggestions.size(); ++i) {
                if (i != 0) {
                    help += i + 1 == suggestions.size() ? " or " : ", ";
                }
                help += suggestions[i];
            }
            help += ".";
            error(loc, "ambiguous promoted member `" + fieldName + "`", help);
        }

        if (lookup.owner.structType) {
            error(loc, "unknown struct field `" + fieldName + "`",
                  "Check the field name, or use a direct method call like "
                  "`obj.method(...)`.");
        }

        auto ownerType = lookup.owner.valueType
                             ? describeResolvedType(lookup.owner.valueType)
                             : std::string("<unknown type>");
        error(loc, "unknown member `" + ownerType + "." + fieldName + "`");
    }

    [[noreturn]] void diagnoseModuleNamespaceValueUse(
        const std::string &moduleName, const location &loc) {
        error(loc, "module namespaces can't be used as runtime values",
              "Access a concrete member like `" + moduleName +
                  ".func(...)` or `" + moduleName + ".Type(...)` instead.");
    }

    [[noreturn]] void diagnoseModuleNamespaceCall(const std::string &moduleName,
                                                  const location &loc) {
        error(loc, "module `" + moduleName + "` does not support call syntax",
              "Call a concrete member like `" + moduleName +
                  ".func(...)` or `" + moduleName + ".Type(...)` instead.");
    }

    HIRExpr *materializeResolvedEntity(const ResolvedEntityRef *binding,
                                       const location &loc,
                                       const std::string &name) {
        if (!binding || !binding->valid()) {
            internalError(
                loc, "missing resolved identifier binding for `" + name + "`",
                "Run name resolution before HIR lowering.");
        }

        switch (binding->kind()) {
            case ResolvedEntityRef::Kind::LocalBinding:
                return makeHIR<HIRValue>(
                    requireBoundObject(binding->localBinding(), loc), loc);
            case ResolvedEntityRef::Kind::GlobalValue: {
                auto *obj = requireGlobalObject(binding->resolvedName(), loc,
                                                "global identifier");
                return makeHIR<HIRValue>(obj, loc);
            }
            case ResolvedEntityRef::Kind::Type: {
                auto *type = requireTypeByName(binding->resolvedName(), loc,
                                               "type identifier");
                return makeHIR<HIRValue>(new TypeObject(type), loc);
            }
            case ResolvedEntityRef::Kind::Invalid:
                break;
        }
        internalError(loc,
                      "unsupported resolved entity kind for `" + name + "`",
                      "This looks like a compiler pipeline bug.");
    }

    CallResolution resolveCall(HIRExpr *callee, CallArgList callArgs,
                               const location &loc) {
        auto resolution = classifyEntity(callee).applyCall(std::move(callArgs));

        if (auto *calleeValue = dynamic_cast<HIRValue *>(callee)) {
            if (auto *typeObject = calleeValue->getValue()->as<TypeObject>()) {
                auto *declaredType = typeObject->declaredType();
                auto *structType = asUnqualified<StructType>(declaredType);
                if (structType) {
                    if (structType->isOpaque()) {
                        error(loc,
                              "opaque struct `" +
                                  describeResolvedType(structType) +
                                  "` cannot be constructed by value",
                              "Use `" + describeResolvedType(structType) +
                                  "*` from an API that owns the storage "
                                  "instead. Opaque structs do not expose "
                                  "fields or value layout.");
                    }
                    resolution.kind = CallResolutionKind::ConstructorCall;
                    resolution.resultEntity = EntityRef::typedValue(structType);
                    return resolution;
                }
                resolution.kind = CallResolutionKind::NotCallable;
                resolution.callee = EntityRef::type(declaredType);
                return resolution;
            }
        }

        if (auto *selector = dynamic_cast<HIRSelector *>(callee);
            selector && selector->isMethodSelector()) {
            auto *structType = selector->getParent()
                                   ? asUnqualified<StructType>(
                                         selector->getParent()->getType())
                                   : nullptr;
            if (!structType) {
                internalError(loc,
                              "selector call parent must be a struct value");
            }
            auto *methodFunc = typeMgr->getMethodFunction(
                structType, toStringRef(selector->getFieldName()));
            auto *funcType = methodFunc ? methodFunc->getType()->as<FuncType>()
                                        : structType->getMethodType(toStringRef(
                                              selector->getFieldName()));
            if (!funcType) {
                internalError(loc, "unknown struct method");
            }
            requireMethodReceiverCompatible(selector, funcType, loc);
            resolution.kind = CallResolutionKind::FunctionCall;
            resolution.callType = funcType;
            resolution.paramNames =
                methodFunc && !methodFunc->paramNames().empty()
                    ? &methodFunc->paramNames()
                    : structType->getMethodParamNames(
                          toStringRef(selector->getFieldName()));
            resolution.argOffset = getMethodCallArgOffset(selector, funcType);
            if (auto *retType = funcType->getRetType()) {
                resolution.resultEntity = EntityRef::typedValue(retType);
            }
            return resolution;
        }

        if (auto *func = getDirectFunctionCallee(callee)) {
            auto *funcType = func->getType()->as<FuncType>();
            resolution.kind = CallResolutionKind::FunctionCall;
            resolution.callType = funcType;
            resolution.paramNames = func && !func->paramNames().empty()
                                        ? &func->paramNames()
                                        : nullptr;
            if (funcType && funcType->getRetType()) {
                resolution.resultEntity =
                    EntityRef::typedValue(funcType->getRetType());
            }
            return resolution;
        }

        if (auto *pointerTarget = getFunctionPointerTarget(callee->getType())) {
            resolution.kind = CallResolutionKind::FunctionPointerCall;
            resolution.callType = pointerTarget;
            if (pointerTarget->getRetType()) {
                resolution.resultEntity =
                    EntityRef::typedValue(pointerTarget->getRetType());
            }
            return resolution;
        }

        if (auto *arrayType = asUnqualified<ArrayType>(callee->getType())) {
            resolution.kind = CallResolutionKind::ArrayIndex;
            resolution.resultEntity =
                EntityRef::typedValue(projectArrayElementType(
                    callee->getType(), arrayType->getElementType()));
            return resolution;
        }
        if (auto *indexableType =
                asUnqualified<IndexablePointerType>(callee->getType())) {
            resolution.kind = CallResolutionKind::ArrayIndex;
            resolution.resultEntity =
                EntityRef::typedValue(indexableType->getElementType());
            return resolution;
        }

        resolution.kind = CallResolutionKind::NotCallable;
        return resolution;
    }

    CallResolutionAttempt resolveCallWithImplicitDeref(
        HIRExpr *callee, const CallArgList &callArgs, const location &loc,
        bool allowImplicitDeref) {
        CallResolutionAttempt attempt;
        attempt.callee = callee;
        attempt.resolution = resolveCall(callee, callArgs, loc);
        if (!allowImplicitDeref ||
            attempt.resolution.kind != CallResolutionKind::NotCallable) {
            return attempt;
        }

        auto *derefCallee = implicitDeref(callee, loc);
        if (!derefCallee) {
            return attempt;
        }

        attempt.callee = derefCallee;
        attempt.resolution = resolveCall(derefCallee, callArgs, loc);
        return attempt;
    }

    [[noreturn]] void diagnoseCallFailure(HIRExpr *callee, const location &loc,
                                          const CallResolution &resolution) {
        if (auto *type = resolution.callee.asType()) {
            if (!asUnqualified<StructType>(type)) {
                error(loc,
                      "constructor calls currently support struct types only",
                      "Use a struct type like `Vec2(...)`. Numeric conversion "
                      "uses `cast[T](expr)`.");
            }
        }

        (void)callee;
        error(loc, "this expression does not support call syntax",
              "Only functions, function pointers, struct constructors, fixed "
              "arrays, and indexable pointers support `(...)` here.");
    }

    Function *requireDeclaredFunction(const location &loc) {
        if (!resolved.hasDeclaredFunction()) {
            internalError(
                loc, "resolved function is missing its stable symbol identity",
                "This looks like a compiler pipeline bug.");
        }
        if (resolved.isMethod()) {
            auto *structType = requireStructTypeByName(
                resolved.methodParentTypeName(), loc, "method parent type");
            auto *func = typeMgr->getMethodFunction(
                structType, toStringRef(resolved.functionName()));
            if (!func) {
                internalError(loc,
                              "resolved method `" +
                                  toStdString(resolved.methodParentTypeName()) +
                                  "." + toStdString(resolved.functionName()) +
                                  "` is missing from the current type table",
                              "Rebuild declarations before reusing this "
                              "resolved module.");
            }
            return func;
        }
        return requireGlobalFunction(resolved.functionName(), loc,
                                     "function declaration");
    }

    HIRExpr *requireExpr(AstNode *node, TypeClass *expectedType = nullptr) {
        auto *expr = analyzeExpr(node, expectedType);
        if (!expr) {
            error(node ? node->loc : location(),
                  "expression did not produce a value");
        }
        return expr;
    }

    HIRExpr *coerceNumericExpr(HIRExpr *expr, TypeClass *targetType,
                               const location &loc, bool explicitRequest) {
        return coerceNumericInitializerExpr(typeMgr, ownerModule, expr,
                                            targetType, loc, explicitRequest);
    }

    HIRExpr *coercePointerExpr(HIRExpr *expr, TypeClass *targetType,
                               const location &loc, bool explicitCast = false) {
        return coercePointerInitializerExpr(typeMgr, ownerModule, expr,
                                            targetType, loc, explicitCast);
    }

    HIRExpr *coerceBitCopyExpr(HIRExpr *expr, TypeClass *targetType,
                               const location &loc) {
        if (!expr || !targetType) {
            return expr;
        }
        auto *sourceType = expr->getType();
        if (!sourceType || sourceType == targetType) {
            return expr;
        }
        if (!canExplicitBitCopy(targetType, sourceType)) {
            error(loc,
                  "raw bit-copy is not available from `" +
                      describeResolvedType(sourceType) + "` to `" +
                      describeResolvedType(targetType) + "`",
                  bitCopyHint());
        }
        return makeHIR<HIRBitCast>(expr, targetType, loc);
    }

    HIRExpr *analyzeCastExpr(AstCastExpr *node) {
        if (!node || !node->targetType || !node->value) {
            error(
                node ? node->loc : location(),
                "builtin cast requires exactly one target type and one value");
        }

        auto *targetType = requireType(node->targetType, node->targetType->loc,
                                       "unknown cast target type");
        auto *value = requireNonCallExpr(node->value);
        if (isNullLiteralExpr(value)) {
            if (!isPointerLikeType(targetType)) {
                error(node->loc,
                      "unsupported builtin cast from `null` to `" +
                          describeResolvedType(targetType) + "`",
                      nullLiteralHint());
            }
            return makeHIR<HIRNullLiteral>(targetType, node->loc);
        }
        auto *sourceType = value ? value->getType() : nullptr;
        if (!sourceType) {
            error(node->loc,
                  "cast source does not produce a typed runtime value",
                  "Cast a runtime value like `cast[i32](x)` instead of a type "
                  "or namespace.");
        }

        if (!isBuiltinCastType(sourceType) || !isBuiltinCastType(targetType)) {
            error(node->loc,
                  "unsupported builtin cast from `" +
                      describeResolvedType(sourceType) + "` to `" +
                      describeResolvedType(targetType) + "`",
                  castDomainHint());
        }

        if (sourceType == targetType) {
            return value;
        }

        if (canExplicitNumericConversion(targetType, sourceType)) {
            return makeHIR<HIRNumericCast>(value, targetType, true, node->loc);
        }

        auto *pointerCast =
            coercePointerExpr(value, targetType, node->loc, true);
        if (pointerCast && pointerCast->getType() == targetType) {
            return pointerCast;
        }

        error(node->loc,
              "unsupported builtin cast from `" +
                  describeResolvedType(sourceType) + "` to `" +
                  describeResolvedType(targetType) + "`",
              numericConversionHint() + " " + pointerConversionHint() + " " +
                  bitCopyHint());
    }

    std::uint64_t requireSizeofByteCount(TypeClass *type, const location &loc,
                                         const std::string &context) {
        auto *storageType = materializeValueType(typeMgr, type);
        if (!storageType) {
            error(loc, context,
                  "Use a concrete type like `sizeof[i32]()` or a typed runtime "
                  "value like `sizeof(x)`.");
        }
        if (storageType->as<FuncType>()) {
            error(loc, "`sizeof` does not support function types",
                  "Use an explicit function pointer type such as "
                  "`sizeof[(i32:)]()` or a function pointer value if you need "
                  "pointer size.");
        }

        const auto byteCount = typeMgr->getTypeAllocSize(storageType);
        if (byteCount == 0) {
            error(loc, "`sizeof` requires a concrete type with known layout",
                  "Opaque extern structs, bare functions, and untyped `null` "
                  "do not have a compile-time size here.");
        }
        return byteCount;
    }

    HIRExpr *analyzeSizeofExpr(AstSizeofExpr *node) {
        if (!node || (!node->hasTypeOperand() && !node->hasValueOperand())) {
            error(node ? node->loc : location(),
                  "builtin `sizeof` requires exactly one operand");
        }

        TypeClass *operandType = nullptr;
        if (node->hasTypeOperand()) {
            operandType = requireType(node->targetType, node->targetType->loc,
                                      "unknown `sizeof` target type");
        } else {
            auto *value = requireNonCallExpr(node->value);
            operandType = value ? value->getType() : nullptr;
            if (!operandType) {
                error(node->loc,
                      "`sizeof` value operand must have a concrete type",
                      "Use `sizeof[T]()` for a type, or pass a typed runtime "
                      "value.");
            }
        }

        const auto byteCount = requireSizeofByteCount(
            operandType, node->loc,
            node->hasTypeOperand()
                ? "`sizeof` target type is not sized"
                : "`sizeof` value operand does not have a known size");
        const auto usizeBytes = typeMgr->getTypeAllocSize(usizeTy);
        const auto usizeBits = static_cast<unsigned>(usizeBytes * 8);
        if (usizeBits < 64 &&
            byteCount > ((std::uint64_t{1} << usizeBits) - 1)) {
            error(
                node->loc,
                "`sizeof` result does not fit in `usize` for the active target",
                "Use a target with a wider pointer size, or avoid requesting "
                "layouts this large.");
        }

        return makeHIR<HIRValue>(new ConstVar(usizeTy, byteCount), node->loc);
    }

    HIRExpr *requireNonCallExpr(AstNode *node,
                                TypeClass *expectedType = nullptr) {
        auto *expr = requireExpr(node, expectedType);
        rejectNonCallMethodSelector(typeMgr, expr);
        return expr;
    }

    CallArgSpec normalizeCallArg(AstNode *node, const location &callLoc) {
        if (!node) {
            error(callLoc, "call argument is missing");
        }

        CallArgSpec spec;
        spec.syntax = node;
        spec.loc = node->loc;
        AstNode *value = node;
        if (auto *namedArg = dynamic_cast<AstNamedCallArg *>(node)) {
            spec.name = toStdString(namedArg->name);
            value = namedArg->value;
        }
        if (auto *refExpr = dynamic_cast<AstRefExpr *>(value)) {
            spec.bindingKind = BindingKind::Ref;
            value = refExpr->expr;
        }
        if (!value) {
            error(node->loc, "call argument is missing its value",
                  "Write arguments like `f(x)`, `f(name=x)`, or `f(ref x)`.");
        }
        spec.value = value;
        return spec;
    }

    CallArgList normalizeCallArgs(const std::vector<AstNode *> *rawArgs,
                                  const location &callLoc) {
        CallArgList normalized;
        if (!rawArgs || rawArgs->empty()) {
            return normalized;
        }
        normalized.reserve(rawArgs->size());
        for (auto *arg : *rawArgs) {
            normalized.push_back(normalizeCallArg(arg, callLoc));
        }
        return normalized;
    }

    static bool isNamedCallArg(const CallArgSpec &arg) {
        return arg.name.has_value();
    }

    static std::string formatAvailableNames(
        const std::vector<std::string> &names, const std::string &noun) {
        if (names.empty()) {
            return "No " + noun + " names are available here.";
        }
        std::ostringstream out;
        out << "Available " << noun << " names are ";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << "`" << names[i] << "`";
        }
        out << ".";
        return out.str();
    }

    enum class FormalCallArgKind {
        ConstructorField,
        FunctionParameter,
        ArrayIndex,
    };

    struct FormalCallArg {
        const string *name = nullptr;
        TypeClass *type = nullptr;
        BindingKind bindingKind = BindingKind::Value;
        FormalCallArgKind kind = FormalCallArgKind::FunctionParameter;
        std::size_t index = 0;
    };

    enum class CallBindingTargetKind {
        Constructor,
        FunctionCall,
        ArrayIndex,
    };

    struct CallBindingOptions {
        location callLoc;
        CallBindingTargetKind targetKind = CallBindingTargetKind::FunctionCall;
        TypeClass *targetType = nullptr;
        bool allowNamedArgs = false;
    };

    struct BoundCallArg {
        CallArgSpec spec;
        HIRExpr *expr = nullptr;
        TypeClass *expectedType = nullptr;
        BindingKind bindingKind = BindingKind::Value;
        std::size_t index = 0;
    };

    static const string *formalCallArgName(const FormalCallArg &formal) {
        return formal.name;
    }

    static std::string describeFormalCallArg(const FormalCallArg &formal) {
        switch (formal.kind) {
            case FormalCallArgKind::ConstructorField:
                return "field `" +
                       (formal.name ? toStdString(*formal.name)
                                    : std::string()) +
                       "`";
            case FormalCallArgKind::FunctionParameter:
                if (formal.name && !formal.name->empty()) {
                    return "parameter `" + toStdString(*formal.name) + "`";
                }
                return "parameter at index " + std::to_string(formal.index);
            case FormalCallArgKind::ArrayIndex:
                return "index at position " + std::to_string(formal.index);
        }
        return "parameter";
    }

    static std::string formalCallArgTypeMismatchContext(
        const FormalCallArg &formal) {
        switch (formal.kind) {
            case FormalCallArgKind::ConstructorField:
                return "constructor field type mismatch for `" +
                       (formal.name ? toStdString(*formal.name)
                                    : std::string()) +
                       "`";
            case FormalCallArgKind::FunctionParameter:
                return "call argument type mismatch at index " +
                       std::to_string(formal.index);
            case FormalCallArgKind::ArrayIndex:
                return "array index type mismatch at index " +
                       std::to_string(formal.index);
        }
        return "call argument type mismatch";
    }

    static std::string formalCallArgMissingRefHint(
        const FormalCallArg &formal) {
        if (formal.kind == FormalCallArgKind::FunctionParameter &&
            formal.name && !formal.name->empty()) {
            return "Pass it as `ref " + toStdString(*formal.name) +
                   " = value` for named calls, or `ref value` positionally.";
        }
        return "Pass it as `ref value`.";
    }

    std::string describeCallTarget(const CallBindingOptions &options) {
        switch (options.targetKind) {
            case CallBindingTargetKind::Constructor:
                return "constructor `" +
                       describeResolvedType(options.targetType) + "`";
            case CallBindingTargetKind::FunctionCall:
                return options.allowNamedArgs ? "function call"
                                              : "this call target";
            case CallBindingTargetKind::ArrayIndex:
                return "array indexing";
        }
        return "this call target";
    }

    static const char *callBindingNameKind(const CallBindingOptions &options) {
        switch (options.targetKind) {
            case CallBindingTargetKind::Constructor:
                return "field";
            case CallBindingTargetKind::FunctionCall:
                return "parameter";
            case CallBindingTargetKind::ArrayIndex:
                return "index";
        }
        return "parameter";
    }

    std::string callBindingCountMismatchLabel(
        const CallBindingOptions &options) {
        switch (options.targetKind) {
            case CallBindingTargetKind::Constructor:
                return "constructor argument count mismatch for `" +
                       describeResolvedType(options.targetType) + "`";
            case CallBindingTargetKind::FunctionCall:
                return "call argument count mismatch";
            case CallBindingTargetKind::ArrayIndex:
                return "array index arity mismatch";
        }
        return "call argument count mismatch";
    }

    std::string disallowRefMessage(const FormalCallArg &formal,
                                   const CallBindingOptions &options) {
        switch (options.targetKind) {
            case CallBindingTargetKind::Constructor:
                return "constructor arguments do not accept `ref`";
            case CallBindingTargetKind::ArrayIndex:
                return "array indexing does not accept `ref` arguments";
            case CallBindingTargetKind::FunctionCall:
                return "value " + describeFormalCallArg(formal) +
                       " cannot be passed with `ref`";
        }
        return "value cannot be passed with `ref`";
    }

    static const char *disallowRefHint(const CallBindingOptions &options) {
        switch (options.targetKind) {
            case CallBindingTargetKind::Constructor:
                return "Constructors copy field values. Remove `ref` from this "
                       "argument.";
            case CallBindingTargetKind::ArrayIndex:
                return "Use positional indices like `a(i, j)` without `ref`.";
            case CallBindingTargetKind::FunctionCall:
                return "Remove `ref` and pass the value directly.";
        }
        return "Remove `ref` and pass the value directly.";
    }

    std::string formatAvailableFormalNames(
        const std::vector<FormalCallArg> &formals,
        const CallBindingOptions &options) {
        std::vector<std::string> names;
        names.reserve(formals.size());
        for (const auto &formal : formals) {
            names.push_back(formal.name ? toStdString(*formal.name)
                                        : std::string());
        }
        return formatAvailableNames(names, callBindingNameKind(options));
    }

    CallArgList collectOrderedCallArgs(
        const CallArgList &normalizedArgs,
        const std::vector<FormalCallArg> &formals,
        const CallBindingOptions &options) {
        CallArgList normalized = normalizedArgs;
        if (normalized.empty()) {
            return normalized;
        }

        bool hasNamedArgs = false;
        for (const auto &arg : normalized) {
            if (isNamedCallArg(arg)) {
                hasNamedArgs = true;
            }
        }

        if (!hasNamedArgs) {
            return normalized;
        }

        if (!options.allowNamedArgs) {
            error(options.callLoc,
                  "named arguments are not supported for " +
                      describeCallTarget(options),
                  "Use positional arguments for this call target.");
        }

        std::unordered_map<std::string, std::size_t> indexByName;
        indexByName.reserve(formals.size());
        for (std::size_t i = 0; i < formals.size(); ++i) {
            if (const auto *name = formalCallArgName(formals[i])) {
                indexByName.emplace(toStdString(*name), i);
            }
        }

        CallArgList ordered(formals.size());
        std::size_t positionalCount = 0;
        bool seenNamedArg = false;
        for (const auto &arg : normalized) {
            if (!isNamedCallArg(arg)) {
                if (seenNamedArg) {
                    error(
                        arg.syntax ? arg.syntax->loc : options.callLoc,
                        "positional arguments must come before named arguments",
                        "Write calls like `name(a, b, x=..., y=...)`, not "
                        "`name(x=..., a)`.");
                }
                if (positionalCount >= ordered.size()) {
                    error(options.callLoc,
                          "call argument count mismatch: expected at most " +
                              std::to_string(formals.size()) + ", got " +
                              std::to_string(normalized.size()));
                }
                ordered[positionalCount++] = arg;
                continue;
            }

            seenNamedArg = true;
            auto found = indexByName.find(*arg.name);
            if (found == indexByName.end()) {
                error(arg.syntax ? arg.syntax->loc : options.callLoc,
                      "unknown " + std::string(callBindingNameKind(options)) +
                          " `" + *arg.name + "` for " +
                          describeCallTarget(options),
                      formatAvailableFormalNames(formals, options));
            }
            if (ordered[found->second].value != nullptr) {
                error(arg.syntax ? arg.syntax->loc : options.callLoc,
                      "duplicate " + std::string(callBindingNameKind(options)) +
                          " `" + *arg.name + "` for " +
                          describeCallTarget(options),
                      "Each " + std::string(callBindingNameKind(options)) +
                          " can only be specified once.");
            }
            ordered[found->second] = arg;
        }

        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (!ordered[i].value) {
                auto *requiredName = formalCallArgName(formals[i]);
                error(options.callLoc,
                      "missing " + std::string(callBindingNameKind(options)) +
                          " `" +
                          (requiredName ? toStdString(*requiredName)
                                        : std::string()) +
                          "` for " + describeCallTarget(options),
                      formatAvailableFormalNames(formals, options));
            }
        }

        return ordered;
    }

    std::vector<BoundCallArg> bindCallArgs(
        const CallArgList &normalizedArgs,
        const std::vector<FormalCallArg> &formals,
        const CallBindingOptions &options) {
        auto orderedArgs =
            collectOrderedCallArgs(normalizedArgs, formals, options);
        if (orderedArgs.size() != formals.size()) {
            error(options.callLoc,
                  callBindingCountMismatchLabel(options) + ": expected " +
                      std::to_string(formals.size()) + ", got " +
                      std::to_string(orderedArgs.size()));
        }

        std::vector<BoundCallArg> boundArgs;
        boundArgs.reserve(orderedArgs.size());
        for (std::size_t i = 0; i < orderedArgs.size(); ++i) {
            const auto &spec = orderedArgs[i];
            const auto &formal = formals[i];
            const auto argLoc =
                spec.value ? spec.value->loc
                           : (spec.syntax ? spec.syntax->loc : options.callLoc);
            if (formal.bindingKind == BindingKind::Ref) {
                if (spec.bindingKind != BindingKind::Ref) {
                    error(spec.syntax ? spec.syntax->loc : options.callLoc,
                          "reference " + describeFormalCallArg(formal) +
                              " must be passed with `ref`",
                          formalCallArgMissingRefHint(formal));
                }
            } else if (spec.bindingKind == BindingKind::Ref) {
                error(spec.syntax ? spec.syntax->loc : options.callLoc,
                      disallowRefMessage(formal, options),
                      disallowRefHint(options));
            }

            auto *expr = requireNonCallExpr(spec.value, formal.type);
            if (formal.bindingKind == BindingKind::Ref) {
                if (!isAddressable(expr)) {
                    error(argLoc,
                          "reference " + describeFormalCallArg(formal) +
                              " expects an addressable value",
                          "Pass a variable, struct field, dereferenced "
                          "pointer, or array indexing expression.");
                }
                if (!canBindReferenceType(formal.type, expr->getType())) {
                    error(argLoc,
                          "reference " + describeFormalCallArg(formal) +
                              " type mismatch: expected " +
                              describeResolvedType(formal.type) + ", got " +
                              describeResolvedType(expr->getType()),
                          "Reference arguments can add const to the view, but "
                          "they cannot drop existing const qualifiers from the "
                          "referenced storage.");
                }
            } else {
                expr = coerceNumericExpr(expr, formal.type, argLoc, false);
                expr = coercePointerExpr(expr, formal.type, argLoc);
                requireCompatibleTypes(
                    argLoc, formal.type, expr->getType(),
                    formalCallArgTypeMismatchContext(formal));
            }

            boundArgs.push_back(
                {spec, expr, formal.type, formal.bindingKind, i});
        }
        return boundArgs;
    }

    std::vector<std::pair<string, TypeClass *>> orderedStructMembers(
        StructType *structType, const location &loc) {
        std::vector<std::pair<string, TypeClass *>> members(
            structType ? structType->getMembers().size() : 0);
        if (!structType) {
            return members;
        }
        for (const auto &entry : structType->getMembers()) {
            const auto index = static_cast<std::size_t>(entry.second.second);
            if (index >= members.size()) {
                internalError(loc,
                              "struct member index is out of range for `" +
                                  describeResolvedType(structType) + "`",
                              "This looks like a type layout bug.");
            }
            members[index] = {entry.first(), entry.second.first};
        }
        return members;
    }

    bool isAddressable(HIRExpr *expr) {
        if (!expr) {
            return false;
        }
        if (auto *value = dynamic_cast<HIRValue *>(expr)) {
            auto *object = value->getValue();
            return object && object->isVariable() && !object->isRegVal();
        }
        if (auto *selector = dynamic_cast<HIRSelector *>(expr)) {
            return selector->isValueFieldSelector() &&
                   isAddressable(selector->getParent());
        }
        if (auto *unary = dynamic_cast<HIRUnaryOper *>(expr)) {
            return unary->getOp() == '*' && unary->getType() != nullptr;
        }
        if (dynamic_cast<HIRIndex *>(expr)) {
            return true;
        }
        return false;
    }

    HIRBlock *analyzeBlock(AstNode *node) {
        auto *block = makeHIR<HIRBlock>(node ? node->loc : location());
        if (!node) {
            return block;
        }

        if (auto *list = node->as<AstStatList>()) {
            for (auto *stmt : list->getBody()) {
                auto *hirNode = analyzeStmt(stmt);
                if (hirNode) {
                    block->push(hirNode);
                }
            }
            return block;
        }

        auto *hirNode = analyzeStmt(node);
        if (hirNode) {
            block->push(hirNode);
        }
        return block;
    }

    HIRNode *analyzeStmt(AstNode *node) {
        if (!node) {
            return nullptr;
        }
        if (node->is<AstStatList>()) {
            return analyzeBlock(node);
        }
        if (auto *varDef = node->as<AstVarDef>()) {
            return analyzeVarDef(varDef);
        }
        if (auto *ret = node->as<AstRet>()) {
            return analyzeRet(ret);
        }
        if (auto *breakNode = node->as<AstBreak>()) {
            return analyzeBreak(breakNode);
        }
        if (auto *continueNode = node->as<AstContinue>()) {
            return analyzeContinue(continueNode);
        }
        if (auto *ifNode = node->as<AstIf>()) {
            return analyzeIf(ifNode);
        }
        if (auto *forNode = node->as<AstFor>()) {
            return analyzeFor(forNode);
        }
        if (node->is<AstStructDecl>() || node->is<AstFuncDecl>() ||
            node->is<AstImport>()) {
            return nullptr;
        }
        auto *expr = requireNonCallExpr(node);
        return expr;
    }

    HIRExpr *analyzeExpr(AstNode *node, TypeClass *expectedType = nullptr) {
        if (!node) {
            return nullptr;
        }
        if (auto *constant = node->as<AstConst>()) {
            return analyzeConst(constant, expectedType);
        }
        if (auto *field = node->as<AstField>()) {
            return analyzeField(field);
        }
        if (auto *funcRef = node->as<AstFuncRef>()) {
            return analyzeFuncRef(funcRef);
        }
        if (auto *assign = node->as<AstAssign>()) {
            return analyzeAssign(assign);
        }
        if (auto *bin = node->as<AstBinOper>()) {
            return analyzeBinOper(bin, expectedType);
        }
        if (auto *unary = node->as<AstUnaryOper>()) {
            return analyzeUnaryOper(unary, expectedType);
        }
        if (auto *refExpr = node->as<AstRefExpr>()) {
            error(refExpr->loc, "`ref` is only valid as a call argument marker",
                  "Use it in calls like `f(ref x)` or `f(ref name = x)`.");
        }
        if (auto *tuple = node->as<AstTupleLiteral>()) {
            return analyzeTupleLiteral(tuple, expectedType);
        }
        if (auto *braceInit = node->as<AstBraceInit>()) {
            return analyzeBraceInit(braceInit, expectedType);
        }
        if (auto *dotLike = node->as<AstDotLike>()) {
            return analyzeDotLike(dotLike);
        }
        if (auto *castExpr = node->as<AstCastExpr>()) {
            return analyzeCastExpr(castExpr);
        }
        if (auto *sizeofExpr = node->as<AstSizeofExpr>()) {
            return analyzeSizeofExpr(sizeofExpr);
        }
        if (auto *call = node->as<AstFieldCall>()) {
            return analyzeCall(call, expectedType);
        }
        error(node->loc, "unsupported AST node in HIR analysis");
    }

    HIRExpr *analyzeConst(AstConst *node, TypeClass *expectedType = nullptr) {
        return analyzeStaticLiteralInitializerExpr(typeMgr, ownerModule, node,
                                                   expectedType);
    }

    HIRExpr *analyzeTupleLiteral(AstTupleLiteral *node,
                                 TypeClass *expectedType) {
        auto *tupleType =
            expectedType ? expectedType->as<TupleType>() : nullptr;
        const auto actualCount = node->items ? node->items->size() : 0;

        std::vector<HIRExpr *> items;
        items.reserve(actualCount);

        if (!tupleType) {
            std::vector<TypeClass *> inferredItemTypes;
            inferredItemTypes.reserve(actualCount);
            for (size_t i = 0; i < actualCount; ++i) {
                auto *item = requireNonCallExpr(node->items->at(i));
                auto *itemType = item ? item->getType() : nullptr;
                if (!itemType) {
                    auto *value = dynamic_cast<HIRValue *>(item);
                    auto *object = value ? value->getValue() : nullptr;
                    if (object && object->as<TypeObject>()) {
                        error(node->items->at(i)->loc,
                              "type names can't be stored as tuple elements",
                              "Use the type in a type annotation, or construct "
                              "a runtime value from it.");
                    }
                    error(node->items->at(i)->loc,
                          "tuple element doesn't produce a storable runtime "
                          "value");
                }
                inferredItemTypes.push_back(itemType);
                items.push_back(item);
            }
            tupleType = typeMgr->getOrCreateTupleType(inferredItemTypes);
        } else {
            const auto &itemTypes = tupleType->getItemTypes();
            if (actualCount != itemTypes.size()) {
                error(node->loc, "tuple literal arity mismatch: expected " +
                                     std::to_string(itemTypes.size()) +
                                     " items, got " +
                                     std::to_string(actualCount));
            }

            for (size_t i = 0; i < actualCount; ++i) {
                auto *item =
                    requireNonCallExpr(node->items->at(i), itemTypes[i]);
                item = coerceNumericExpr(item, itemTypes[i],
                                         node->items->at(i)->loc, false);
                item = coercePointerExpr(item, itemTypes[i],
                                         node->items->at(i)->loc);
                requireCompatibleTypes(node->items->at(i)->loc, itemTypes[i],
                                       item->getType(),
                                       "tuple element type mismatch at index " +
                                           std::to_string(i));
                items.push_back(item);
            }
        }
        return makeHIR<HIRTupleLiteral>(std::move(items), tupleType, node->loc);
    }

    class InitialListLowering {
        FunctionAnalyzer &owner;

        struct InitialList;

        struct InitialListItem {
            location loc;
            AstNode *expr = nullptr;
            std::unique_ptr<InitialList> nested;
        };

        struct InitialList {
            location loc;
            std::vector<InitialListItem> items;
        };

        struct InferredArrayShape {
            TypeClass *elementType = nullptr;
            std::vector<std::size_t> extents;
        };

        TypeClass *mergeInferredElementType(TypeClass *current, TypeClass *next,
                                            const location &loc) {
            if (!current) {
                return next;
            }
            if (!next || current == next) {
                return current;
            }
            if (auto *common =
                    commonNumericType(owner.typeMgr, current, next)) {
                return common;
            }
            if (canImplicitPointerViewConversion(current, next)) {
                return current;
            }
            if (canImplicitPointerViewConversion(next, current)) {
                return next;
            }
            owner.error(
                loc,
                "cannot infer a common array element type from `" +
                    describeResolvedType(current) + "` and `" +
                    describeResolvedType(next) + "`",
                numericConversionHint() + " " + pointerConversionHint());
        }

        ArrayType *buildArrayTypeFromShape(const InferredArrayShape &shape,
                                           const location &loc) {
            if (!shape.elementType || shape.extents.empty()) {
                return nullptr;
            }
            TypeClass *current = shape.elementType;
            for (auto it = shape.extents.rbegin(); it != shape.extents.rend();
                 ++it) {
                std::vector<AstNode *> dims;
                dims.push_back(owner.makeStaticDimensionNode(*it, loc));
                current =
                    owner.typeMgr->createArrayType(current, std::move(dims));
            }
            return asUnqualified<ArrayType>(current);
        }

        InferredArrayShape inferArrayShape(const InitialList &initList) {
            if (initList.items.empty()) {
                owner.error(
                    initList.loc,
                    "cannot infer an array type from an empty brace "
                    "initializer",
                    "Write an explicit type like `var a i32[2] = {}`, or "
                    "provide at least one element such as `var a = {1, 2}`.");
            }

            InferredArrayShape shape;
            shape.extents.push_back(initList.items.size());

            const bool hasNested =
                std::any_of(initList.items.begin(), initList.items.end(),
                            [](const InitialListItem &item) {
                                return item.nested != nullptr;
                            });

            if (!hasNested) {
                for (const auto &item : initList.items) {
                    auto *expr = owner.requireNonCallExpr(item.expr);
                    if (!expr->getType()) {
                        owner.error(
                            item.loc,
                            "array element does not produce an inferable "
                            "runtime type",
                            "Write an explicit array type, or use elements "
                            "with concrete runtime value types.");
                    }
                    shape.elementType = mergeInferredElementType(
                        shape.elementType, expr->getType(), item.loc);
                }
                return shape;
            }

            std::optional<std::vector<std::size_t>> childExtents;
            for (const auto &item : initList.items) {
                if (!item.nested) {
                    owner.error(item.loc,
                                "array initializer cannot mix nested and "
                                "non-nested elements",
                                "Use either `{1, 2}` for a flat array, or "
                                "`{{1, 2}, {3, 4}}` for a nested array.");
                }
                auto childShape = inferArrayShape(*item.nested);
                if (!childExtents) {
                    childExtents = childShape.extents;
                } else if (*childExtents != childShape.extents) {
                    owner.error(item.loc,
                                "nested array initializer rows must have a "
                                "consistent shape",
                                "Keep each nested brace group the same length, "
                                "for example `{{1, 2}, {3, 4}}`.");
                }
                shape.elementType = mergeInferredElementType(
                    shape.elementType, childShape.elementType, item.loc);
            }
            if (childExtents) {
                shape.extents.insert(shape.extents.end(), childExtents->begin(),
                                     childExtents->end());
            }
            return shape;
        }

        std::vector<AstNode *> consumeArrayOuterDimension(
            const std::vector<AstNode *> &dims) {
            std::vector<AstNode *> remaining;
            remaining.reserve(dims.size());
            bool consumed = false;
            const bool legacyPrefix = isLegacyArrayDimensionPrefix(dims);
            for (auto *dim : dims) {
                if (dim == nullptr) {
                    continue;
                }
                if (!consumed) {
                    consumed = true;
                    continue;
                }
                remaining.push_back(dim);
            }
            if (legacyPrefix && remaining.size() > 1) {
                remaining.insert(remaining.begin(), nullptr);
            }
            return remaining;
        }

        TypeClass *arrayInitChildType(ArrayType *arrayType) {
            if (!arrayType) {
                return nullptr;
            }
            bool ok = false;
            auto dims = arrayType->staticDimensions(&ok);
            if (!ok || dims.empty()) {
                return nullptr;
            }
            if (dims.size() == 1) {
                return arrayType->getElementType();
            }
            auto childDims =
                consumeArrayOuterDimension(arrayType->getDimensions());
            return owner.typeMgr->createArrayType(arrayType->getElementType(),
                                                  std::move(childDims));
        }

        std::unique_ptr<InitialList> buildInitialList(AstBraceInit *node) {
            if (!node) {
                return nullptr;
            }
            auto initList = std::make_unique<InitialList>();
            initList->loc = node->loc;
            if (!node->items || node->items->empty()) {
                return initList;
            }
            initList->items.reserve(node->items->size());
            for (auto *rawItem : *node->items) {
                auto *braceItem = dynamic_cast<AstBraceInitItem *>(rawItem);
                if (!braceItem || !braceItem->value) {
                    owner.error(node->loc,
                                "array initializer contains an invalid item",
                                "Each item must be an expression or a nested "
                                "brace group.");
                }
                InitialListItem item;
                item.loc = braceItem->value->loc;
                if (auto *nested =
                        dynamic_cast<AstBraceInit *>(braceItem->value)) {
                    item.nested = buildInitialList(nested);
                } else {
                    item.expr = braceItem->value;
                }
                initList->items.push_back(std::move(item));
            }
            return initList;
        }

        HIRExpr *materializeArrayInit(const InitialList &initList,
                                      ArrayType *arrayType,
                                      const location &loc) {
            if (!arrayType) {
                owner.internalError(loc,
                                    "invalid array initializer materialization",
                                    "This looks like a compiler pipeline bug.");
            }
            if (!arrayType->hasStaticLayout()) {
                owner.error(
                    loc,
                    "array initializers currently require fixed explicit "
                    "dimensions",
                    "Use positive integer literal dimensions. Dimension "
                    "inference and dynamic sizes are not implemented yet.");
            }

            bool ok = false;
            auto dims = arrayType->staticDimensions(&ok);
            if (!ok || dims.empty()) {
                owner.error(
                    loc,
                    "array initializers currently require fixed explicit "
                    "dimensions",
                    "Use positive integer literal dimensions. Dimension "
                    "inference and dynamic sizes are not implemented yet.");
            }
            const auto outerExtent = static_cast<std::size_t>(dims.front());
            auto *childType = arrayInitChildType(arrayType);
            if (!childType) {
                owner.error(loc,
                            "array initializer is missing its element type",
                            "This looks like a compiler pipeline bug.");
            }

            std::vector<HIRExpr *> items;
            if (initList.items.size() > outerExtent) {
                owner.error(
                    loc,
                    "array initializer has too many elements: expected at "
                    "most " +
                        std::to_string(outerExtent) + ", got " +
                        std::to_string(initList.items.size()),
                    "Remove extra elements or increase the array dimension.");
            }
            if (isFixedArrayOfFunctionPointerValues(arrayType) &&
                initList.items.size() != outerExtent) {
                owner.error(loc,
                            "function pointer arrays require full "
                            "initialization: expected exactly " +
                                std::to_string(outerExtent) +
                                " elements, got " +
                                std::to_string(initList.items.size()),
                            "Initialize every slot explicitly. Missing "
                            "elements would become null function pointers.");
            }

            items.reserve(initList.items.size());
            for (std::size_t i = 0; i < initList.items.size(); ++i) {
                const auto &item = initList.items[i];
                if (item.nested) {
                    auto *childArrayType = asUnqualified<ArrayType>(childType);
                    if (!childArrayType) {
                        owner.error(item.loc,
                                    "array initializer nesting is deeper than "
                                    "the array shape",
                                    "Remove this brace level, or make the "
                                    "target element type another array.");
                    }
                    items.push_back(materializeArrayInit(
                        *item.nested, childArrayType, item.loc));
                    continue;
                }

                if (asUnqualified<ArrayType>(childType)) {
                    owner.error(item.loc,
                                "array initializer expects a nested brace "
                                "group at index " +
                                    std::to_string(i),
                                "Write nested rows like `{{1, 2}, {3, 4}}` so "
                                "the brace structure matches the array shape.");
                }

                auto *value = owner.requireNonCallExpr(item.expr, childType);
                value =
                    owner.coerceNumericExpr(value, childType, item.loc, false);
                value = owner.coercePointerExpr(value, childType, item.loc);
                owner.requireCompatibleTypes(
                    item.loc, childType, value->getType(),
                    "array initializer element type mismatch at index " +
                        std::to_string(i));
                items.push_back(value);
            }

            return owner.makeHIR<HIRArrayInit>(std::move(items), arrayType,
                                               loc);
        }

        HIRExpr *materializeInitList(const InitialList &initList,
                                     TypeClass *expectedType,
                                     const location &loc) {
            if (!expectedType) {
                auto inferredShape = inferArrayShape(initList);
                expectedType = buildArrayTypeFromShape(inferredShape, loc);
                if (!expectedType) {
                    owner.internalError(
                        loc, "failed to build inferred array type",
                        "This looks like a compiler pipeline bug.");
                }
            }
            if (auto *arrayType = asUnqualified<ArrayType>(expectedType)) {
                return materializeArrayInit(initList, arrayType, loc);
            }
            if (auto *structType = asUnqualified<StructType>(expectedType)) {
                owner.error(
                    loc,
                    "brace initialization currently applies to arrays only",
                    "For structs, call the type directly like `" +
                        describeResolvedType(structType) +
                        "(...)` using positional or named arguments. "
                        "`initial_list` remains an internal "
                        "array-initialization interface.");
            }
            owner.error(loc,
                        "brace initializer currently supports arrays only when "
                        "the target type is already known",
                        "Write a declaration like `var matrix i32[4][5] = "
                        "{{1}, {2}}`, or call a struct type like `Vec2(x=1, "
                        "y=2)` for struct construction.");
        }

    public:
        explicit InitialListLowering(FunctionAnalyzer &owner) : owner(owner) {}

        HIRExpr *analyze(AstBraceInit *node, TypeClass *expectedType) {
            auto initList = buildInitialList(node);
            return materializeInitList(*initList, expectedType, node->loc);
        }
    };

    HIRExpr *analyzeBraceInit(AstBraceInit *node, TypeClass *expectedType) {
        return InitialListLowering(*this).analyze(node, expectedType);
    }

    HIRExpr *analyzeField(AstField *node) {
        auto *binding = resolved.field(node);
        if (binding && binding->kind() == ResolvedEntityRef::Kind::Module) {
            diagnoseModuleNamespaceValueUse(toStdString(node->name), node->loc);
        }
        return materializeResolvedEntity(binding, node->loc,
                                         toStdString(node->name));
    }

    HIRExpr *analyzeResolvedDotLike(AstDotLike *node) {
        auto *binding = resolved.dotLike(node);
        if (!binding || !binding->valid()) {
            return nullptr;
        }
        return materializeResolvedEntity(binding, node->loc,
                                         describeMemberOwnerSyntax(node));
    }

    HIRExpr *analyzeFuncRef(AstFuncRef *node) {
        auto *binding = resolved.functionRef(node);
        if (!binding || !binding->valid()) {
            internalError(node->loc,
                          "missing resolved function reference for `" +
                              toStdString(node->name) + "`",
                          "Run name resolution before HIR lowering.");
        }

        auto *func = requireGlobalFunction(binding->resolvedName(), node->loc,
                                           "function reference");
        auto *funcType =
            func->getType() ? func->getType()->as<FuncType>() : nullptr;
        if (!funcType) {
            internalError(node->loc,
                          "invalid resolved function reference target `" +
                              toStdString(node->name) + "`",
                          "This looks like a compiler pipeline bug.");
        }

        const auto &expectedArgTypes = funcType->getArgTypes();
        const auto actualArgCount = node->argTypes ? node->argTypes->size() : 0;
        if (expectedArgTypes.size() != actualArgCount) {
            error(node->loc,
                  "function reference parameter count mismatch for `" +
                      toStdString(node->name) + "`: expected " +
                      std::to_string(expectedArgTypes.size()) + ", got " +
                      std::to_string(actualArgCount));
        }

        for (size_t i = 0; i < actualArgCount; ++i) {
            auto actualBindingKind =
                funcParamBindingKind(node->argTypes->at(i));
            auto *actualType = requireType(
                unwrapFuncParamType(node->argTypes->at(i)),
                node->argTypes->at(i)->loc,
                "unknown function reference parameter type at index " +
                    std::to_string(i) + " for `" + toStdString(node->name) +
                    "`: " + describeTypeNode(node->argTypes->at(i)));
            auto *expectedType = expectedArgTypes[i];
            auto expectedBindingKind = funcType->getArgBindingKind(i);
            if (actualBindingKind != expectedBindingKind ||
                actualType != expectedType) {
                auto expectedDescription =
                    (expectedBindingKind == BindingKind::Ref ? "ref " : "") +
                    describeResolvedType(expectedType);
                auto actualDescription =
                    (actualBindingKind == BindingKind::Ref ? "ref " : "") +
                    describeResolvedType(actualType);
                error(node->argTypes->at(i)->loc,
                      "function reference parameter type mismatch at index " +
                          std::to_string(i) + " for `" +
                          toStdString(node->name) + "`: expected " +
                          expectedDescription + ", got " + actualDescription);
            }
        }

        auto *pointerType = typeMgr->createPointerType(funcType);
        auto *value = pointerType->newObj(Object::REG_VAL | Object::READONLY);
        value->bindllvmValue(func->getllvmValue());
        return makeHIR<HIRValue>(value, node->loc);
    }

    HIRExpr *analyzeAssign(AstAssign *node) {
        auto *left = requireNonCallExpr(node->left);
        if (!isAddressable(left)) {
            error(node->left ? node->left->loc : node->loc,
                  "assignment expects an addressable value on the left side",
                  "You can assign to variables, struct fields, dereferenced "
                  "pointers, or array indexing expressions.");
        }
        if (left && !isFullyWritableValueType(left->getType())) {
            errorReadOnlyAssignmentTarget(
                node->left ? node->left->loc : node->loc, left->getType());
        }
        auto *right =
            requireNonCallExpr(node->right, left ? left->getType() : nullptr);
        right = coerceNumericExpr(right, left ? left->getType() : nullptr,
                                  node->loc, false);
        right = coercePointerExpr(right, left ? left->getType() : nullptr,
                                  node->loc);
        auto *leftType = left->getType();
        auto *rightType = right->getType();
        if (!leftType || !rightType ||
            !isByteCopyCompatible(leftType, rightType)) {
            requireCompatibleTypes(node->loc, leftType, rightType,
                                   "assignment type mismatch");
        }
        return makeHIR<HIRAssign>(left, right, node->loc);
    }

    HIRExpr *analyzeBinOper(AstBinOper *node,
                            TypeClass *expectedType = nullptr) {
        TypeClass *contextualOperandType =
            expectedType && isNumericType(expectedType) ? expectedType
                                                        : nullptr;
        auto *left = requireNonCallExpr(node->left, contextualOperandType);
        auto *right = requireNonCallExpr(
            node->right, contextualOperandType
                             ? contextualOperandType
                             : (left ? left->getType() : nullptr));
        if (isNullLiteralExpr(left) || isNullLiteralExpr(right)) {
            if (node->op != Parser::token::LOGIC_EQUAL &&
                node->op != Parser::token::LOGIC_NOT_EQUAL) {
                error(node->loc, "`null` only supports pointer equality checks",
                      nullLiteralHint());
            }
            if (isNullLiteralExpr(left) && right &&
                isPointerLikeType(right->getType())) {
                left =
                    coercePointerExpr(left, right->getType(), node->left->loc);
            }
            if (isNullLiteralExpr(right) && left &&
                isPointerLikeType(left->getType())) {
                right =
                    coercePointerExpr(right, left->getType(), node->right->loc);
            }
            if (isNullLiteralExpr(left) && isNullLiteralExpr(right)) {
                error(node->loc,
                      "`null` comparison requires a concrete pointer operand",
                      "Compare a pointer value against `null`, for example `if "
                      "p == null`.");
            }
            if ((isNullLiteralExpr(left) &&
                 !isPointerLikeType(left->getType())) ||
                (isNullLiteralExpr(right) &&
                 !isPointerLikeType(right->getType())) ||
                (isNullLiteralExpr(left) &&
                 !isPointerLikeType(right->getType())) ||
                (isNullLiteralExpr(right) &&
                 !isPointerLikeType(left->getType()))) {
                error(node->loc,
                      "`null` can only be compared with pointer values",
                      nullLiteralHint());
            }
        }
        if (left->getType() != right->getType()) {
            auto *leftConst = node->left ? node->left->as<AstConst>() : nullptr;
            if (leftConst && leftConst->isDefaultFloatLiteral() &&
                isFloatType(right->getType())) {
                left = requireNonCallExpr(node->left, right->getType());
            }
        }
        if (left->getType() != right->getType()) {
            auto *rightConst =
                node->right ? node->right->as<AstConst>() : nullptr;
            if (rightConst && rightConst->isDefaultFloatLiteral() &&
                isFloatType(left->getType())) {
                right = requireNonCallExpr(node->right, left->getType());
            }
        }
        if (left->getType() != right->getType()) {
            if (auto *commonType = commonNumericType(typeMgr, left->getType(),
                                                     right->getType())) {
                left =
                    coerceNumericExpr(left, commonType, node->left->loc, false);
                right = coerceNumericExpr(right, commonType, node->right->loc,
                                          false);
            }
        }
        if (left->getType() != right->getType() &&
            (node->op == Parser::token::LOGIC_EQUAL ||
             node->op == Parser::token::LOGIC_NOT_EQUAL)) {
            auto *leftType = left->getType();
            auto *rightType = right->getType();
            if (isRawMemoryPointerType(leftType) &&
                isIndexablePointerType(rightType)) {
                right = coercePointerExpr(right, leftType, node->right->loc);
            } else if (isIndexablePointerType(leftType) &&
                       isRawMemoryPointerType(rightType)) {
                left = coercePointerExpr(left, rightType, node->left->loc);
            }
        }
        auto binding = operatorResolver.resolveBinary(
            node->op, left->getType(), right->getType(), node->loc);
        return makeHIR<HIRBinOper>(binding, left, right, binding.resultType,
                                   node->loc);
    }

    HIRExpr *analyzeUnaryOper(AstUnaryOper *node,
                              TypeClass *expectedType = nullptr) {
        if (isSupportedStaticLiteralInitializerExpr(node)) {
            return analyzeStaticLiteralInitializerExpr(typeMgr, ownerModule,
                                                       node, expectedType);
        }
        if (node->op == '-') {
            auto *constant = node->expr ? node->expr->as<AstConst>() : nullptr;
            if (constant && constant->isUnaryMinusOnlySignedMinLiteral()) {
                switch (constant->getType()) {
                    case AstConst::Type::I8:
                        return makeHIR<HIRValue>(
                            new ConstVar(
                                i8Ty, std::numeric_limits<std::int8_t>::min()),
                            node->loc);
                    case AstConst::Type::I16:
                        return makeHIR<HIRValue>(
                            new ConstVar(
                                i16Ty,
                                std::numeric_limits<std::int16_t>::min()),
                            node->loc);
                    case AstConst::Type::I32:
                        return makeHIR<HIRValue>(
                            new ConstVar(
                                i32Ty,
                                std::numeric_limits<std::int32_t>::min()),
                            node->loc);
                    case AstConst::Type::I64:
                        return makeHIR<HIRValue>(
                            new ConstVar(
                                i64Ty,
                                std::numeric_limits<std::int64_t>::min()),
                            node->loc);
                    default:
                        break;
                }
            }
        }
        TypeClass *contextualOperandType =
            expectedType && isNumericType(expectedType) ? expectedType
                                                        : nullptr;
        auto *value = requireNonCallExpr(node->expr, contextualOperandType);
        auto binding = operatorResolver.resolveUnary(
            node->op, value->getType(), isAddressable(value), node->loc);
        return makeHIR<HIRUnaryOper>(binding, value, binding.resultType,
                                     node->loc);
    }

    HIRNode *analyzeVarDef(AstVarDef *node) {
        if (auto *typeNode = node ? node->getTypeNode() : nullptr) {
            validateTypeNodeLayout(typeNode);
        }
        const bool isRefBinding = node->isRefBinding();

        TypeClass *type = nullptr;
        if (auto *typeNode = node->getTypeNode()) {
            type =
                requireType(typeNode, typeNode->loc, "unknown variable type");
            rejectBareFunctionStorage(type, node);
            rejectOpaqueStructStorage(type, node);
            rejectConstVariableStorage(type, node);
            rejectUninitializedFunctionPointerValueStorage(type, node);
        } else if (isRefBinding) {
            error(node->loc,
                  "reference binding `" + toStdString(node->getName()) +
                      "` requires an explicit type annotation",
                  "Write `ref name Type = value` so the alias target type is "
                  "explicit.");
        }

        HIRExpr *init = nullptr;
        if (node->withInitVal()) {
            init = isRefBinding ? requireNonCallExpr(node->getInitVal(), type)
                                : requireExpr(node->getInitVal(), type);
        }

        if (isRefBinding && !init) {
            error(node->loc,
                  "reference binding `" + toStdString(node->getName()) +
                      "` requires an initializer",
                  "Bind it to an addressable value like `ref a i32 = x`.");
        }

        if (type) {
            if (init) {
                if (isRefBinding) {
                    if (!isAddressable(init)) {
                        error(node->getInitVal() ? node->getInitVal()->loc
                                                 : node->loc,
                              "reference binding expects an addressable value",
                              "Bind references to variables, struct fields, "
                              "dereferenced pointers, or array indexing "
                              "expressions.");
                    }
                    if (!canBindReferenceType(type, init->getType())) {
                        error(node->loc,
                              "reference binding type mismatch for `" +
                                  toStdString(node->getName()) +
                                  "`: expected " + describeResolvedType(type) +
                                  ", got " +
                                  describeResolvedType(init->getType()),
                              "Reference bindings can add const to the alias "
                              "view, but they cannot drop existing const "
                              "qualifiers from the referenced storage.");
                    }
                } else {
                    init = coerceNumericExpr(init, type, node->loc, false);
                    init = coercePointerExpr(init, type, node->loc);
                    requireCompatibleTypes(node->loc, type, init->getType(),
                                           "initializer type mismatch for `" +
                                               toStdString(node->getName()) +
                                               "`");
                }
            }
        } else if (init) {
            rejectMethodSelectorStorage(typeMgr, init, node);
            type = materializeValueType(typeMgr, init->getType());
            if (!type) {
                if (isNullLiteralExpr(init)) {
                    error(node->loc,
                          "cannot infer the type of `" +
                              toStdString(node->getName()) + "` from `null`",
                          "Add an explicit pointer type such as `var p i32* = "
                          "null`.");
                }
                auto *value = dynamic_cast<HIRValue *>(init);
                auto *object = value ? value->getValue() : nullptr;
                if (object && object->as<TypeObject>()) {
                    error(node->loc,
                          "type names can't be stored as runtime values",
                          "Call the type like `Vec2(...)`, or use it in a type "
                          "annotation.");
                }
                error(
                    node->loc,
                    "this expression doesn't produce a storable runtime value");
            }
            rejectBareFunctionStorage(type, node);
            rejectOpaqueStructStorage(type, node);
        } else {
            error(node->loc,
                  "cannot infer the type of `" + toStdString(node->getName()) +
                      "` without an initializer",
                  "Add an explicit type annotation or provide an initializer.");
        }

        if (node->isReadOnlyBinding()) {
            if (isRefBinding) {
                internalError(
                    node->loc,
                    "read-only variable binding unexpectedly used with `ref`",
                    "Keep `const name = expr` as a value binding only.");
            }
            type = typeMgr->createConstType(type);
        }

        auto *binding = resolved.variable(node);
        if (!binding) {
            internalError(node->loc,
                          "missing resolved variable binding for `" +
                              toStdString(node->getName()) + "`",
                          "Run name resolution before HIR lowering.");
        }
        auto *obj =
            type->newObj(Object::VARIABLE |
                         (isRefBinding ? Object::REF_ALIAS : Object::EMPTY));
        bindObject(binding, obj);
        return makeHIR<HIRVarDef>(binding->name(), obj, init, node->loc);
    }

    HIRNode *analyzeRet(AstRet *node) {
        auto *retType = hirFunc->getFuncType()->getRetType();
        HIRExpr *expr = nullptr;
        if (node->expr) {
            expr = requireNonCallExpr(node->expr, retType);
            if (!retType) {
                error(node->loc, "unexpected return value in void function");
            }
            expr = coerceNumericExpr(expr, retType, node->expr->loc, false);
            expr = coercePointerExpr(expr, retType, node->expr->loc);
            requireCompatibleTypes(node->loc, retType, expr->getType(),
                                   "return type mismatch");
        } else if (retType) {
            error(node->loc, "missing return value");
        }
        return makeHIR<HIRRet>(expr, node->loc);
    }

    HIRNode *analyzeBreak(AstBreak *node) {
        if (loopDepth <= 0) {
            error(node->loc, "`break` can only appear inside `for` loops");
        }
        return makeHIR<HIRBreak>(node->loc);
    }

    HIRNode *analyzeContinue(AstContinue *node) {
        if (loopDepth <= 0) {
            error(node->loc, "`continue` can only appear inside `for` loops");
        }
        return makeHIR<HIRContinue>(node->loc);
    }

    HIRNode *analyzeIf(AstIf *node) {
        auto *cond = requireNonCallExpr(node->condition);
        if (!isTruthyScalarType(cond->getType())) {
            error(node->condition ? node->condition->loc : node->loc,
                  "if condition expects a scalar truthy value",
                  "Use `bool`, numeric values, or pointers in condition "
                  "expressions.");
        }
        auto *thenBlock = analyzeBlock(node->then);
        auto *elseBlock = node->hasElse() ? analyzeBlock(node->els) : nullptr;
        return makeHIR<HIRIf>(cond, thenBlock, elseBlock, node->loc);
    }

    HIRNode *analyzeFor(AstFor *node) {
        auto *cond = requireNonCallExpr(node->expr);
        if (!isTruthyScalarType(cond->getType())) {
            error(
                node->expr ? node->expr->loc : node->loc,
                "for condition expects a scalar truthy value",
                "Use `bool`, numeric values, or pointers in loop conditions.");
        }
        ++loopDepth;
        auto *body = analyzeBlock(node->body);
        --loopDepth;
        auto *elseBlock = node->hasElse() ? analyzeBlock(node->els) : nullptr;
        return makeHIR<HIRFor>(cond, body, elseBlock, node->loc);
    }

    HIRExpr *analyzeDotLike(AstDotLike *node) {
        if (auto *resolvedDotLike = analyzeResolvedDotLike(node)) {
            return resolvedDotLike;
        }

        auto *parent = requireExpr(node->parent);
        auto fieldName = toStdString(node->field->text);
        auto attempt = lookupMemberWithImplicitDeref(
            parent, fieldName, node->loc, !isExplicitDerefSyntax(node->parent));
        if (auto *expr = materializeMemberExpr(attempt.parent, fieldName,
                                               attempt.lookup, node->loc)) {
            return expr;
        }
        diagnoseMemberLookupFailure(attempt.lookup, fieldName, node->loc,
                                    describeMemberOwnerSyntax(node->parent));
    }

    HIRExpr *analyzeCall(AstFieldCall *node,
                         TypeClass *expectedType = nullptr) {
        auto normalizedArgs = normalizeCallArgs(node->args, node->loc);
        HIRExpr *callee = nullptr;
        if (auto *fieldNode =
                node->value ? node->value->as<AstField>() : nullptr) {
            auto *binding = resolved.field(fieldNode);
            if (binding && binding->kind() == ResolvedEntityRef::Kind::Module) {
                diagnoseModuleNamespaceCall(toStdString(fieldNode->name),
                                            node->loc);
            }
        }
        if (auto *dotLikeNode =
                node->value ? node->value->as<AstDotLike>() : nullptr) {
            if (auto *resolvedDotLike = analyzeResolvedDotLike(dotLikeNode)) {
                callee = resolvedDotLike;
            } else {
                auto *receiver = requireExpr(dotLikeNode->parent);
                auto fieldName = toStdString(dotLikeNode->field->text);
                auto attempt = lookupMemberWithImplicitDeref(
                    receiver, fieldName, node->loc,
                    !isExplicitDerefSyntax(dotLikeNode->parent));
                if (attempt.lookup.result.kind ==
                    LookupResultKind::InjectedMember) {
                    assert(attempt.lookup.injectedMember.has_value());
                    if (attempt.lookup.injectedMember->kind ==
                        InjectedMemberKind::BitCopy) {
                        if (node->args && !node->args->empty()) {
                            error(node->loc,
                                  "raw bit-copy member `" + fieldName +
                                      "` does not take arguments",
                                  "Call it as `<expr>." + fieldName + "()`.");
                        }
                        return coerceBitCopyExpr(
                            attempt.parent,
                            attempt.lookup.injectedMember->resultType,
                            node->loc);
                    }
                }
                if (auto *resolvedCallee = materializeMemberExpr(
                        attempt.parent, fieldName, attempt.lookup,
                        dotLikeNode->loc, true)) {
                    callee = resolvedCallee;
                } else {
                    diagnoseMemberLookupFailure(
                        attempt.lookup, fieldName, dotLikeNode->loc,
                        describeMemberOwnerSyntax(dotLikeNode->parent));
                }
            }
        }

        if (!callee) {
            callee = requireExpr(node->value);
        }
        auto callAttempt =
            resolveCallWithImplicitDeref(callee, normalizedArgs, node->loc,
                                         !isExplicitDerefSyntax(node->value));
        callee = callAttempt.callee;
        auto resolution = std::move(callAttempt.resolution);
        switch (resolution.kind) {
            case CallResolutionKind::ConstructorCall: {
                auto *structType =
                    resolution.callee.asType()
                        ? asUnqualified<StructType>(resolution.callee.asType())
                        : nullptr;
                if (!structType) {
                    internalError(
                        node->loc,
                        "constructor resolution is missing its struct type",
                        "This looks like a compiler pipeline bug.");
                }

                auto members = orderedStructMembers(structType, node->loc);
                std::vector<FormalCallArg> formals;
                formals.reserve(members.size());
                for (std::size_t i = 0; i < members.size(); ++i) {
                    formals.push_back({&members[i].first, members[i].second,
                                       BindingKind::Value,
                                       FormalCallArgKind::ConstructorField, i});
                }
                auto boundArgs =
                    bindCallArgs(resolution.args, formals,
                                 {node->loc, CallBindingTargetKind::Constructor,
                                  structType, true});

                std::vector<HIRExpr *> fields;
                fields.reserve(boundArgs.size());
                for (const auto &arg : boundArgs) {
                    fields.push_back(arg.expr);
                }
                return makeHIR<HIRStructLiteral>(std::move(fields), structType,
                                                 node->loc);
            }
            case CallResolutionKind::ArrayIndex: {
                auto *arrayType = asUnqualified<ArrayType>(callee->getType());
                auto *indexableType =
                    asUnqualified<IndexablePointerType>(callee->getType());
                const auto indexArity = arrayType ? arrayType->indexArity()
                                                  : (indexableType ? 1u : 0u);
                auto *elementType = resolution.resultEntity.valueType();
                if (!arrayType && !indexableType) {
                    internalError(
                        node->loc,
                        "array index resolution is missing its indexable type",
                        "This looks like a compiler pipeline bug.");
                }
                if (arrayType && !arrayType->hasStaticLayout()) {
                    error(node->loc,
                          "array indexing requires fixed explicit dimensions "
                          "or an indexable pointer",
                          "Use positive integer literal dimensions like "
                          "`i32[4]`, or an indexable pointer like `T[*]` and "
                          "write `ptr(i)`.");
                }
                std::vector<FormalCallArg> formals;
                formals.reserve(indexArity);
                for (size_t i = 0; i < indexArity; ++i) {
                    formals.push_back({nullptr, i32Ty, BindingKind::Value,
                                       FormalCallArgKind::ArrayIndex, i});
                }
                auto boundArgs =
                    bindCallArgs(resolution.args, formals,
                                 {node->loc, CallBindingTargetKind::ArrayIndex,
                                  nullptr, false});
                const auto actualArgCount = boundArgs.size();
                std::vector<HIRExpr *> args;
                args.reserve(actualArgCount);
                for (const auto &arg : boundArgs) {
                    args.push_back(arg.expr);
                }
                return makeHIR<HIRIndex>(callee, std::move(args), elementType,
                                         node->loc);
            }
            case CallResolutionKind::FunctionCall:
            case CallResolutionKind::FunctionPointerCall: {
                auto *funcType = resolution.callType;
                if (!funcType) {
                    internalError(
                        node->loc,
                        "call resolution is missing its function type",
                        "This looks like a compiler pipeline bug.");
                }

                const auto &paramTypes = funcType->getArgTypes();
                std::vector<FormalCallArg> formals;
                formals.reserve(paramTypes.size() - resolution.argOffset);
                for (size_t i = 0; i + resolution.argOffset < paramTypes.size();
                     ++i) {
                    const string *paramName =
                        resolution.paramNames &&
                                i < resolution.paramNames->size()
                            ? &resolution.paramNames->at(i)
                            : nullptr;
                    formals.push_back(
                        {paramName, paramTypes[i + resolution.argOffset],
                         funcType->getArgBindingKind(i + resolution.argOffset),
                         FormalCallArgKind::FunctionParameter, i});
                }
                auto boundArgs = bindCallArgs(
                    resolution.args, formals,
                    {node->loc, CallBindingTargetKind::FunctionCall, nullptr,
                     resolution.paramNames && !resolution.paramNames->empty()});

                std::vector<HIRExpr *> args;
                args.reserve(boundArgs.size());
                for (const auto &arg : boundArgs) {
                    args.push_back(arg.expr);
                }

                auto *retType = funcType->getRetType();
                return makeHIR<HIRCall>(callee, std::move(args), retType,
                                        node->loc);
            }
            case CallResolutionKind::NotCallable:
                diagnoseCallFailure(callee, node->loc, resolution);
            default:
                internalError(
                    node->loc,
                    "call resolution produced an unsupported result kind",
                    "This looks like a compiler pipeline bug.");
        }
    }

public:
    FunctionAnalyzer(TypeTable *typeMgr, GlobalScope *global,
                     HIRModule *ownerModule, const CompilationUnit *unit,
                     const ResolvedFunction &resolved)
        : typeMgr(typeMgr),
          global(global),
          unit(unit),
          resolved(resolved),
          operatorResolver(typeMgr),
          ownerModule(ownerModule),
          hirFunc(nullptr) {}

private:
    void prepareFunctionShell() {
        if (resolved.isTopLevelEntry()) {
            hirFunc =
                makeHIR<HIRFunc>(getOrCreateTopLevelEntry(global, typeMgr),
                                 getOrCreateMainType(typeMgr), resolved.loc(),
                                 true, resolved.guaranteedReturn());
        } else {
            auto *lofunc = requireDeclaredFunction(resolved.loc());
            auto *funcType =
                lofunc->getType() ? lofunc->getType()->as<FuncType>() : nullptr;
            if (!funcType) {
                internalError(resolved.loc(),
                              "resolved function type is invalid",
                              "This looks like a compiler pipeline bug.");
            }
            hirFunc = makeHIR<HIRFunc>(
                llvm::cast<llvm::Function>(lofunc->getllvmValue()), funcType,
                resolved.loc(), false, resolved.guaranteedReturn());
        }
    }

    void bindSelfIfNeeded() {
        if (resolved.hasSelfBinding()) {
            if (!resolved.isMethod()) {
                internalError(
                    resolved.loc(),
                    "resolved self binding is missing its method parent",
                    "This looks like a compiler pipeline bug.");
            }
            auto *methodParent =
                requireStructTypeByName(resolved.methodParentTypeName(),
                                        resolved.loc(), "method parent type");
            auto *decl = resolved.decl();
            auto *receiverPointee =
                decl && decl->receiverAccess == AccessKind::GetSet
                    ? static_cast<TypeClass *>(methodParent)
                    : static_cast<TypeClass *>(
                          typeMgr->createConstType(methodParent));
            auto *selfType = typeMgr->createPointerType(receiverPointee);
            auto *selfObj = selfType->newObj(Object::VARIABLE);
            bindObject(resolved.selfBinding(), selfObj);
            hirFunc->setSelfBinding(HIRBinding{
                resolved.selfBinding()->name(),
                resolved.selfBinding()->bindingKind(),
                selfObj,
                resolved.selfBinding()->loc(),
            });
        }
    }

    void bindParameters() {
        for (auto *paramBinding : resolved.params()) {
            auto *decl = paramBinding ? paramBinding->parameterDecl() : nullptr;
            if (!decl) {
                internalError(
                    resolved.loc(),
                    "resolved parameter binding is missing its declaration",
                    "This looks like a compiler pipeline bug.");
            }
            auto *type = requireType(
                decl->typeNode,
                decl->typeNode ? decl->typeNode->loc : paramBinding->loc(),
                "unknown function argument type for `" +
                    toStdString(paramBinding->name()) + "`");
            auto *argObj =
                type->newObj(Object::VARIABLE |
                             (paramBinding->isRefBinding() ? Object::REF_ALIAS
                                                           : Object::EMPTY));
            bindObject(paramBinding, argObj);
            hirFunc->addParam(HIRBinding{
                paramBinding->name(),
                paramBinding->bindingKind(),
                argObj,
                paramBinding->loc(),
            });
        }
    }

    void analyzeBody() {
        if (!resolved.body()) {
            hirFunc->setBody(nullptr);
            return;
        }
        hirFunc->setBody(analyzeBlock(const_cast<AstNode *>(resolved.body())));
    }

public:
    HIRFunc *analyze() {
        if (hirFunc != nullptr) {
            return hirFunc;
        }
        prepareFunctionShell();
        bindSelfIfNeeded();
        bindParameters();
        analyzeBody();
        return hirFunc;
    }
};

class ModuleAnalyzer {
    GlobalScope *global;
    TypeTable *typeMgr;
    const CompilationUnit *unit;
    std::unique_ptr<HIRModule> module = std::make_unique<HIRModule>();

public:
    explicit ModuleAnalyzer(GlobalScope *global, const CompilationUnit *unit)
        : global(global), typeMgr(requireTypeTable(global)), unit(unit) {}

    std::unique_ptr<HIRModule> analyze(const ResolvedModule &resolvedModule) {
        for (const auto &resolvedFunction : resolvedModule.functions()) {
            FunctionAnalyzer analyzer(typeMgr, global, module.get(), unit,
                                      *resolvedFunction);
            module->addFunction(analyzer.analyze());
        }
        return std::move(module);
    }
};

}  // namespace analysis_impl

std::unique_ptr<HIRModule>
analyzeModule(GlobalScope *global, const ResolvedModule &resolved,
              const CompilationUnit *unit) {
    return analysis_impl::ModuleAnalyzer(global, unit).analyze(resolved);
}

}  // namespace lona
