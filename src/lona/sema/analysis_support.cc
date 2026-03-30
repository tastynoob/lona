#include "lona/sema/analysis_support.hh"
#include "lona/abi/abi.hh"
#include "lona/ast/array_dim.hh"
#include "lona/ast/astnode.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/call_target_tools.hh"
#include "lona/sema/initializer_semantics.hh"
#include "lona/sema/operator_resolver.hh"
#include "lona/sym/func.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include "parser.hh"
#include <cassert>

namespace lona {
namespace analysis_impl {

std::string
describeResolvedFuncType(FuncType *type, size_t argOffset) {
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

}  // namespace analysis_impl
}  // namespace lona
