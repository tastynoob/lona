#include "lona/sema/hir.hh"
#include "lona/ast/astnode.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/array_dim.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/injected_member.hh"
#include "lona/sema/operator_resolver.hh"
#include "lona/sym/func.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include "parser.hh"
#include <cassert>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

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

std::string
describeResolvedType(TypeClass *type);

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
    name += ")";
    if (type->getRetType() != nullptr) {
        name += " ";
        name += describeResolvedType(type->getRetType());
    }
    return name;
}

std::string
describeResolvedType(TypeClass *type) {
    if (type == nullptr) {
        return "<unknown type>";
    }
    if (auto *pointer = type->as<PointerType>()) {
        if (auto *func = pointer->getPointeeType()->as<FuncType>()) {
            std::string name = "(";
            const auto &argTypes = func->getArgTypes();
            for (size_t i = 0; i < argTypes.size(); ++i) {
                if (i != 0) {
                    name += ", ";
                }
                if (func->getArgBindingKind(i) == BindingKind::Ref) {
                    name += "ref ";
                }
                name += describeResolvedType(argTypes[i]);
            }
            name += ")*";
            if (func->getRetType() != nullptr) {
                name += " ";
                name += describeResolvedType(func->getRetType());
            }
            return name;
        }
        return describeResolvedType(pointer->getPointeeType()) + "*";
    }
    if (auto *array = type->as<ArrayType>()) {
        return toStdString(array->full_name);
    }
    if (auto *func = type->as<FuncType>()) {
        return describeResolvedFuncType(func);
    }
    return toStdString(type->full_name);
}

std::string
describeStorageType(TypeClass *type, AstVarDef *node) {
    if (node != nullptr && node->getTypeNode() != nullptr) {
        return describeTypeNode(node->getTypeNode());
    }
    return describeResolvedType(type);
}

[[noreturn]] void
errorInvalidArrayDimension(const location &loc) {
    error(loc,
          "fixed-dimension arrays require positive integer literal sizes",
          "Use explicit sizes like `i32[4][5]` or `i32[5,4]`. Dimension inference and non-constant sizes are not implemented yet.");
}

[[noreturn]] void
errorUnsupportedUnsizedArray(const location &loc, TypeNode *node) {
    error(loc,
          "unsized array syntax is not implemented yet: " +
              describeTypeNode(node, "<unknown type>"),
          "Use fixed explicit dimensions like `i32[4]` or an explicit pointer type like `i32*`. `T[]` remains reserved syntax and has no stable ABI yet.");
}

void
validateTypeNodeLayout(TypeNode *node) {
    if (!node) {
        return;
    }
    if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
        validateTypeNodeLayout(param->type);
        return;
    }
    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        validateTypeNodeLayout(pointer->base);
        return;
    }
    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        validateTypeNodeLayout(array->base);
        if (hasUnsizedArrayDimensions(array->dim)) {
            errorUnsupportedUnsizedArray(array->loc, array);
        }
        for (auto *dimension : array->dim) {
            std::int64_t value = 0;
            if (!tryExtractArrayDimension(dimension, value) || value <= 0) {
                errorInvalidArrayDimension(dimension ? dimension->loc : array->loc);
            }
        }
        return;
    }
    if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
        for (auto *item : tuple->items) {
            validateTypeNodeLayout(item);
        }
        return;
    }
    if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
        for (auto *arg : func->args) {
            validateTypeNodeLayout(arg);
        }
        validateTypeNodeLayout(func->ret);
        return;
    }
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
numericConversionHint() {
    return "Integer-to-integer and float-to-float convert implicitly. Integer/float cross-conversion requires an explicit `.toXXX()` call like `1.tof32()` or `1.5.toi32()`.";
}

std::string
bitCopyHint() {
    return "Use `.tobits()` when you want raw bit-copy behavior. It returns `u8[N]`, and `u8[N].toXXX()` converts those bytes back with truncation or zero-fill.";
}

bool
isReservedInitialListTypeNode(TypeNode *node) {
    auto *base = dynamic_cast<BaseTypeNode *>(node);
    if (!base) {
        return false;
    }
    return toStdString(base->name) == "initial_list";
}

[[noreturn]] void
errorReservedInitialListType(const location &loc) {
    error(loc,
          "`initial_list` is a compiler-internal initialization interface",
          "Use brace initialization like `{1, 2, 3}` instead. User-visible generic `initial_list<T>` support is not implemented.");
}

std::string
describeInjectedMemberHelp(TypeClass *receiverType, const std::string &memberName) {
    return "Call injected members directly as `<expr>." + memberName +
           "(...)`. Built-in numeric conversion members are injected on numeric values.";
}

FuncType *
getMethodSelectorType(TypeTable *typeMgr, HIRSelector *selector) {
    if (!selector || selector->getType() != nullptr) {
        return nullptr;
    }

    auto *parentType = selector->getParent() ? selector->getParent()->getType() : nullptr;
    auto *structType = parentType ? parentType->as<StructType>() : nullptr;
    if (!structType) {
        return nullptr;
    }

    auto *func = typeMgr ? typeMgr->getMethodFunction(
        structType, llvm::StringRef(selector->getFieldName())) : nullptr;
    return func && func->getType() ? func->getType()->as<FuncType>() : nullptr;
}

FuncType *
getFunctionPointerTarget(TypeClass *type) {
    auto *pointerType = type ? type->as<PointerType>() : nullptr;
    return pointerType ? pointerType->getPointeeType()->as<FuncType>() : nullptr;
}

Function *
getDirectFunctionCallee(HIRExpr *callee) {
    auto *calleeValue = dynamic_cast<HIRValue *>(callee);
    auto *value = calleeValue ? calleeValue->getValue() : nullptr;
    return value ? value->as<Function>() : nullptr;
}

size_t
getMethodCallArgOffset(HIRSelector *selector, FuncType *type) {
    if (!selector || !type) {
        return 0;
    }

    auto *parentType = selector->getParent() ? selector->getParent()->getType() : nullptr;
    const auto &argTypes = type->getArgTypes();
    if (!argTypes.empty() && parentType != nullptr && argTypes.front() == parentType) {
        return 1;
    }
    return 0;
}

std::string
describeMethodSelectorType(HIRSelector *selector, FuncType *type) {
    if (!selector || !type) {
        return "<unknown type>";
    }

    return describeResolvedFuncType(type, getMethodCallArgOffset(selector, type));
}

void
rejectMethodSelectorStorage(TypeTable *typeMgr, HIRExpr *expr, AstVarDef *node) {
    auto *selector = dynamic_cast<HIRSelector *>(expr);
    auto *funcType = getMethodSelectorType(typeMgr, selector);
    if (!selector || !funcType || !node) {
        return;
    }

    error(node->loc,
          "unsupported bare function variable type for `" +
              toStdString(node->getName()) + "`: " +
              describeMethodSelectorType(selector, funcType),
          "Store an explicit function pointer instead of a bare method selector.");
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
resolveInjectedMemberBinding(TypeTable *typeMgr, HIRExpr *receiver,
                             const std::string &memberName) {
    if (!typeMgr || !receiver) {
        return std::nullopt;
    }
    return resolveInjectedMember(typeMgr, receiver->getType(),
                                 llvm::StringRef(memberName));
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
    error(node->loc,
          "unsupported bare function variable type for `" +
              toStdString(node->getName()) + "`: " +
              describeStorageType(type, node),
          "Use an explicit function pointer type like `(T1, T2)* Ret` instead.");
}

void
rejectUninitializedFunctionDerivedStorage(AstVarDef *node) {
    if (!node || node->withInitVal()) {
        return;
    }

    auto *typeNode = node->getTypeNode();
    if (!typeNode) {
        return;
    }
    if (findFuncPtrTypeNode(typeNode) == nullptr) {
        return;
    }

    error(node->loc,
          "function pointer variable type for `" +
              toStdString(node->getName()) + "` requires initializer: " +
              describeTypeNode(typeNode),
          "Initialize function pointers at the point of definition.");
}

TypeTable *
requireTypeTable(Scope *scope) {
    assert(scope);
    auto *typeMgr = scope->types();
    assert(typeMgr);
    return typeMgr;
}

std::string
mainFunctionTypeName() {
    return "f_i32";
}

FuncType *
getOrCreateMainType(TypeTable *typeMgr) {
    return typeMgr->getOrCreateFunctionType({}, i32Ty);
}

llvm::Function *
getOrCreateTopLevelEntry(GlobalScope *global, TypeTable *typeMgr) {
    auto *mainType = getOrCreateMainType(typeMgr);
    auto moduleName = global->module.getName();
    std::string entryName = moduleName.str() + ".main";
    if (auto *existing = global->module.getFunction(entryName)) {
        return existing;
    }

    return llvm::Function::Create(
        typeMgr->getLLVMFunctionType(mainType),
        llvm::Function::ExternalLinkage, llvm::Twine(entryName), global->module);
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
    location currentLocation;
    bool hasCurrentLocation = false;

    class ScopedLocation {
        FunctionAnalyzer &owner;
        location savedLocation;
        bool savedHasLocation = false;

    public:
        ScopedLocation(FunctionAnalyzer &owner, const location &loc)
            : owner(owner),
              savedLocation(owner.currentLocation),
              savedHasLocation(owner.hasCurrentLocation) {
            owner.currentLocation = loc;
            owner.hasCurrentLocation = true;
        }

        ~ScopedLocation() {
            owner.currentLocation = savedLocation;
            owner.hasCurrentLocation = savedHasLocation;
        }
    };

    [[noreturn]] void error(const std::string &message,
                            const std::string &hint = std::string()) {
        if (hasCurrentLocation) {
            lona::error(currentLocation, message, hint);
        }
        lona::error(message);
    }

    [[noreturn]] void error(const location &loc, const std::string &message,
                            const std::string &hint = std::string()) {
        lona::error(loc, message, hint);
    }

    [[noreturn]] void internalError(const std::string &message,
                                    const std::string &hint = std::string()) {
        if (hasCurrentLocation) {
            throw DiagnosticError(DiagnosticError::Category::Internal,
                                  currentLocation, message, hint);
        }
        throw DiagnosticError(DiagnosticError::Category::Internal, message, hint);
    }

    [[noreturn]] void internalError(const location &loc, const std::string &message,
                                    const std::string &hint = std::string()) {
        throw DiagnosticError(DiagnosticError::Category::Internal, loc, message, hint);
    }

    TypeClass *requireType(TypeNode *node, const std::string &context) {
        validateTypeNodeLayout(node);
        if (isReservedInitialListTypeNode(node)) {
            errorReservedInitialListType(node->loc);
        }
        auto *type = unit ? unit->resolveType(typeMgr, node) : typeMgr->getType(node);
        if (!type) {
            error(currentLocation, context);
        }
        return type;
    }

    void requireCompatibleTypes(const location &loc, TypeClass *expectedType,
                                TypeClass *actualType,
                                const std::string &context) {
        if (expectedType && actualType && isByteCopyCompatible(expectedType, actualType)) {
            return;
        }
        error(loc, context + ": expected " + describeResolvedType(expectedType) +
                        ", got " + describeResolvedType(actualType),
              numericConversionHint() + " " + bitCopyHint());
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

    ObjectPtr requireBoundObject(const ResolvedLocalBinding *binding,
                                 const location &loc) {
        if (!binding) {
            internalError(loc, "missing resolved local binding",
                          "Run name resolution before HIR lowering.");
        }
        auto found = bindingObjects.find(binding);
        if (found == bindingObjects.end()) {
            internalError(loc,
                          "resolved local binding `" + binding->name() +
                              "` was not materialized before use",
                          "This looks like a compiler pipeline bug.");
        }
        return found->second;
    }

    ObjectPtr requireGlobalObject(const std::string &name, const location &loc,
                                  const std::string &context) {
        auto *obj = global->getObj(llvm::StringRef(name));
        if (!obj) {
            internalError(loc,
                          "resolved " + context + " `" + name +
                              "` is missing from the current global scope",
                          "Rebuild declarations before reusing this resolved module.");
        }
        return obj;
    }

    Function *requireGlobalFunction(const std::string &name, const location &loc,
                                    const std::string &context) {
        auto *obj = requireGlobalObject(name, loc, context);
        auto *func = obj->as<Function>();
        if (!func) {
            internalError(loc,
                          "resolved " + context + " `" + name +
                              "` no longer refers to a function",
                          "Rebuild declarations before reusing this resolved module.");
        }
        return func;
    }

    StructType *requireStructTypeByName(const std::string &name,
                                        const location &loc,
                                        const std::string &context) {
        auto *type = typeMgr->getType(llvm::StringRef(name));
        auto *structType = type ? type->as<StructType>() : nullptr;
        if (!structType) {
            internalError(loc,
                          "resolved " + context + " `" + name +
                              "` is missing from the current type table",
                          "Rebuild declarations before reusing this resolved module.");
        }
        return structType;
    }

    TypeClass *requireTypeByName(const std::string &name, const location &loc,
                                 const std::string &context) {
        auto *type = typeMgr->getType(llvm::StringRef(name));
        if (!type) {
            internalError(loc,
                          "resolved " + context + " `" + name +
                              "` is missing from the current type table",
                          "Rebuild declarations before reusing this resolved module.");
        }
        return type;
    }

    Function *requireDeclaredFunction(const location &loc) {
        if (!resolved.hasDeclaredFunction()) {
            internalError(loc,
                          "resolved function is missing its stable symbol identity",
                          "This looks like a compiler pipeline bug.");
        }
        if (resolved.isMethod()) {
            auto *structType = requireStructTypeByName(
                resolved.methodParentTypeName(), loc, "method parent type");
            auto *func =
                typeMgr->getMethodFunction(structType,
                                           llvm::StringRef(resolved.functionName()));
            if (!func) {
                internalError(loc,
                              "resolved method `" + resolved.methodParentTypeName() +
                                  "." + resolved.functionName() +
                                  "` is missing from the current type table",
                              "Rebuild declarations before reusing this resolved module.");
            }
            return func;
        }
        return requireGlobalFunction(resolved.functionName(), loc,
                                     "function declaration");
    }

    HIRExpr *requireExpr(AstNode *node, TypeClass *expectedType = nullptr) {
        auto *expr = analyzeExpr(node, expectedType);
        if (!expr) {
            error(node ? node->loc : currentLocation,
                  "expression did not produce a value");
        }
        return expr;
    }

    HIRExpr *coerceNumericExpr(HIRExpr *expr, TypeClass *targetType,
                               const location &loc, bool explicitRequest) {
        if (!expr || !targetType) {
            return expr;
        }
        auto *sourceType = expr->getType();
        if (!sourceType || sourceType == targetType) {
            return expr;
        }
        if (explicitRequest) {
            if (canExplicitNumericConversion(targetType, sourceType)) {
                return makeHIR<HIRNumericCast>(expr, targetType, true, loc);
            }
            error(loc,
                  "explicit numeric conversion is not available from `" +
                      describeResolvedType(sourceType) + "` to `" +
                      describeResolvedType(targetType) + "`",
                  numericConversionHint());
        }
        if (canImplicitNumericConversion(targetType, sourceType)) {
            return makeHIR<HIRNumericCast>(expr, targetType, false, loc);
        }
        return expr;
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

    HIRExpr *requireNonCallExpr(AstNode *node, TypeClass *expectedType = nullptr) {
        auto *expr = requireExpr(node, expectedType);
        rejectNonCallMethodSelector(typeMgr, expr);
        return expr;
    }

    struct CallArgSpec {
        AstNode *syntax = nullptr;
        AstNode *value = nullptr;
        BindingKind bindingKind = BindingKind::Value;
        std::optional<std::string> name;
    };

    CallArgSpec normalizeCallArg(AstNode *node, const location &callLoc) {
        if (!node) {
            error(callLoc, "call argument is missing");
        }

        CallArgSpec spec;
        spec.syntax = node;
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
            error(node->loc,
                  "call argument is missing its value",
                  "Write arguments like `f(x)`, `f(name=x)`, or `f(ref x)`.");
        }
        spec.value = value;
        return spec;
    }

    static bool isNamedCallArg(const CallArgSpec &arg) {
        return arg.name.has_value();
    }

    static std::string describeCallParameter(const std::vector<std::string> *paramNames,
                                             std::size_t index) {
        if (paramNames && index < paramNames->size() && !paramNames->at(index).empty()) {
            return "parameter `" + paramNames->at(index) + "`";
        }
        return "parameter at index " + std::to_string(index);
    }

    static std::string explicitRefCallHint(const std::vector<std::string> *paramNames,
                                           std::size_t index) {
        if (paramNames && index < paramNames->size() && !paramNames->at(index).empty()) {
            return "Pass it as `ref " + paramNames->at(index) +
                   " = value` for named calls, or `ref value` positionally.";
        }
        return "Pass it as `ref value`.";
    }

    static std::string formatAvailableNames(const std::vector<std::string> &names,
                                            const std::string &noun) {
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

    std::vector<CallArgSpec> collectOrderedCallArgs(
        const std::vector<AstNode *> *rawArgs,
        const std::vector<std::string> &orderedNames,
        const location &callLoc, const std::string &targetLabel,
        const std::string &nameKind, bool allowNamedArgs) {
        std::vector<CallArgSpec> normalized;
        if (!rawArgs || rawArgs->empty()) {
            return normalized;
        }
        normalized.reserve(rawArgs->size());
        for (auto *arg : *rawArgs) {
            normalized.push_back(normalizeCallArg(arg, callLoc));
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

        if (!allowNamedArgs) {
            error(callLoc,
                  "named arguments are not supported for " + targetLabel,
                  "Use positional arguments for this call target.");
        }

        std::unordered_map<std::string, std::size_t> indexByName;
        indexByName.reserve(orderedNames.size());
        for (std::size_t i = 0; i < orderedNames.size(); ++i) {
            indexByName.emplace(orderedNames[i], i);
        }

        std::vector<CallArgSpec> ordered(orderedNames.size());
        std::size_t positionalCount = 0;
        bool seenNamedArg = false;
        for (const auto &arg : normalized) {
            if (!isNamedCallArg(arg)) {
                if (seenNamedArg) {
                    error(arg.syntax ? arg.syntax->loc : callLoc,
                          "positional arguments must come before named arguments",
                          "Write calls like `name(a, b, x=..., y=...)`, not `name(x=..., a)`.");
                }
                if (positionalCount >= ordered.size()) {
                    error(callLoc,
                          "call argument count mismatch: expected at most " +
                              std::to_string(orderedNames.size()) + ", got " +
                              std::to_string(rawArgs->size()));
                }
                ordered[positionalCount++] = arg;
                continue;
            }

            seenNamedArg = true;
            auto found = indexByName.find(*arg.name);
            if (found == indexByName.end()) {
                error(arg.syntax ? arg.syntax->loc : callLoc,
                      "unknown " + nameKind + " `" + *arg.name +
                          "` for " + targetLabel,
                      formatAvailableNames(orderedNames, nameKind));
            }
            if (ordered[found->second].value != nullptr) {
                error(arg.syntax ? arg.syntax->loc : callLoc,
                      "duplicate " + nameKind + " `" + *arg.name +
                          "` for " + targetLabel,
                      "Each " + nameKind + " can only be specified once.");
            }
            ordered[found->second] = arg;
        }

        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (!ordered[i].value) {
                error(callLoc,
                      "missing " + nameKind + " `" + orderedNames[i] + "` for " +
                          targetLabel,
                      formatAvailableNames(orderedNames, nameKind));
            }
        }

        return ordered;
    }

    std::vector<std::pair<std::string, TypeClass *>> orderedStructMembers(
        StructType *structType, const location &loc) {
        std::vector<std::pair<std::string, TypeClass *>> members(
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
            members[index] = {entry.first().str(), entry.second.first};
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
            return selector->getType() != nullptr && isAddressable(selector->getParent());
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
        ScopedLocation scopedLocation(*this, node ? node->loc : location());
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
        ScopedLocation scopedLocation(*this, node ? node->loc : location());
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
        if (auto *ifNode = node->as<AstIf>()) {
            return analyzeIf(ifNode);
        }
        if (auto *forNode = node->as<AstFor>()) {
            return analyzeFor(forNode);
        }
        if (node->is<AstStructDecl>() || node->is<AstFuncDecl>()) {
            return nullptr;
        }
        auto *expr = requireNonCallExpr(node);
        return expr;
    }

    HIRExpr *analyzeExpr(AstNode *node, TypeClass *expectedType = nullptr) {
        ScopedLocation scopedLocation(*this, node ? node->loc : location());
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
            error(refExpr->loc,
                  "`ref` is only valid as a call argument marker",
                  "Use it in calls like `f(ref x)` or `f(ref name = x)`.");
        }
        if (auto *tuple = node->as<AstTupleLiteral>()) {
            return analyzeTupleLiteral(tuple, expectedType);
        }
        if (auto *braceInit = node->as<AstBraceInit>()) {
            return analyzeBraceInit(braceInit, expectedType);
        }
        if (auto *selector = node->as<AstSelector>()) {
            return analyzeSelector(selector);
        }
        if (auto *call = node->as<AstFieldCall>()) {
            return analyzeCall(call, expectedType);
        }
        error("unsupported AST node in HIR analysis");
    }

    HIRExpr *analyzeConst(AstConst *node, TypeClass *expectedType = nullptr) {
        switch (node->getType()) {
        case AstConst::Type::INT32:
            return makeHIR<HIRValue>(new ConstVar(i32Ty, *node->getBuf<int32_t>()),
                                     node->loc);
        case AstConst::Type::BOOL:
            return makeHIR<HIRValue>(new ConstVar(boolTy, *node->getBuf<bool>()),
                                     node->loc);
        case AstConst::Type::FP64: {
            auto value = *node->getBuf<double>();
            if (expectedType == f32Ty) {
                return makeHIR<HIRValue>(new ConstVar(f32Ty, static_cast<float>(value)),
                                         node->loc);
            }
            if (expectedType == nullptr || expectedType == f64Ty) {
                return makeHIR<HIRValue>(new ConstVar(f64Ty, value), node->loc);
            }
            error(node->loc,
                  "floating-point literal doesn't match the expected target type",
                  "Use a `f32` or `f64` destination. For numeric conversion, call an explicit helper like `1.5.toi32()`. For raw bits, call `.tobits()` and keep the resulting `u8[N]` array.");
        }
        case AstConst::Type::STRING:
            error(node->loc,
                  "string literals are reserved, but runtime string semantics are not implemented yet",
                  "String syntax is intentionally kept as a placeholder until the runtime representation is defined.");
        default:
            error(node->loc, "unsupported constant literal");
        }
    }

    HIRExpr *analyzeTupleLiteral(AstTupleLiteral *node, TypeClass *expectedType) {
        auto *tupleType = expectedType ? expectedType->as<TupleType>() : nullptr;
        if (!tupleType) {
            error(node->loc,
                  "tuple literals need an explicit tuple target type",
                  "Write a declaration like `var pair <i32, bool> = (1, true)`, or pass the literal to a parameter that already expects a tuple type.");
        }

        const auto &itemTypes = tupleType->getItemTypes();
        const auto actualCount = node->items ? node->items->size() : 0;
        if (actualCount != itemTypes.size()) {
            error(node->loc,
                  "tuple literal arity mismatch: expected " +
                      std::to_string(itemTypes.size()) + " items, got " +
                      std::to_string(actualCount));
        }

        std::vector<HIRExpr *> items;
        items.reserve(actualCount);
        for (size_t i = 0; i < actualCount; ++i) {
            auto *item = requireNonCallExpr(node->items->at(i), itemTypes[i]);
            item = coerceNumericExpr(item, itemTypes[i], node->items->at(i)->loc,
                                     false);
            requireCompatibleTypes(node->items->at(i)->loc, itemTypes[i], item->getType(),
                                   "tuple element type mismatch at index " +
                                       std::to_string(i));
            items.push_back(item);
        }
        return makeHIR<HIRTupleLiteral>(std::move(items), tupleType, node->loc);
    }

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

    std::vector<AstNode *> consumeArrayOuterDimension(const std::vector<AstNode *> &dims) {
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
        auto childDims = consumeArrayOuterDimension(arrayType->getDimensions());
        return typeMgr->createArrayType(arrayType->getElementType(), std::move(childDims));
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
                error(node->loc,
                      "array initializer contains an invalid item",
                      "Each item must be an expression or a nested brace group.");
            }
            InitialListItem item;
            item.loc = braceItem->value->loc;
            if (auto *nested = dynamic_cast<AstBraceInit *>(braceItem->value)) {
                item.nested = buildInitialList(nested);
            } else {
                item.expr = braceItem->value;
            }
            initList->items.push_back(std::move(item));
        }
        return initList;
    }

    HIRExpr *materializeArrayInit(const InitialList &initList, ArrayType *arrayType,
                                  const location &loc) {
        if (!arrayType) {
            error("invalid array initializer materialization");
        }
        if (!arrayType->hasStaticLayout()) {
            error(loc,
                  "array initializers currently require fixed explicit dimensions",
                  "Use positive integer literal dimensions. Dimension inference and dynamic sizes are not implemented yet.");
        }

        bool ok = false;
        auto dims = arrayType->staticDimensions(&ok);
        if (!ok || dims.empty()) {
            error(loc,
                  "array initializers currently require fixed explicit dimensions",
                  "Use positive integer literal dimensions. Dimension inference and dynamic sizes are not implemented yet.");
        }
        const auto outerExtent = static_cast<std::size_t>(dims.front());
        auto *childType = arrayInitChildType(arrayType);
        if (!childType) {
            error(loc,
                  "array initializer is missing its element type",
                  "This looks like a compiler pipeline bug.");
        }

        std::vector<HIRExpr *> items;
        if (initList.items.size() > outerExtent) {
            error(loc,
                  "array initializer has too many elements: expected at most " +
                      std::to_string(outerExtent) + ", got " +
                      std::to_string(initList.items.size()),
                  "Remove extra elements or increase the array dimension.");
        }

        items.reserve(initList.items.size());
        for (std::size_t i = 0; i < initList.items.size(); ++i) {
            const auto &item = initList.items[i];
            if (item.nested) {
                auto *childArrayType = childType->as<ArrayType>();
                if (!childArrayType) {
                    error(item.loc,
                          "array initializer nesting is deeper than the array shape",
                          "Remove this brace level, or make the target element type another array.");
                }
                items.push_back(materializeArrayInit(*item.nested, childArrayType,
                                                     item.loc));
                continue;
            }

            if (childType->as<ArrayType>()) {
                error(item.loc,
                      "array initializer expects a nested brace group at index " +
                          std::to_string(i),
                      "Write nested rows like `{{1, 2}, {3, 4}}` so the brace structure matches the array shape.");
            }

            auto *value = requireNonCallExpr(item.expr, childType);
            value = coerceNumericExpr(value, childType, item.loc, false);
            requireCompatibleTypes(item.loc, childType, value->getType(),
                                   "array initializer element type mismatch at index " +
                                       std::to_string(i));
            items.push_back(value);
        }

        return makeHIR<HIRArrayInit>(std::move(items), arrayType, loc);
    }

    HIRExpr *materializeInitList(const InitialList &initList, TypeClass *expectedType,
                                 const location &loc) {
        if (!expectedType) {
            error("invalid initializer-list materialization");
        }
        if (auto *arrayType = expectedType->as<ArrayType>()) {
            return materializeArrayInit(initList, arrayType, loc);
        }
        if (auto *structType = expectedType->as<StructType>()) {
            error(loc,
                  "brace initialization currently applies to arrays only",
                  "For structs, call the type directly like `" +
                      describeResolvedType(structType) +
                      "(...)` using positional or named arguments. `initial_list` remains an internal array-initialization interface.");
        }
        error(loc,
              "brace initializer currently supports arrays only when the target type is already known",
              "Write a declaration like `var matrix i32[4][5] = {{1}, {2}}`, or call a struct type like `Vec2(x=1, y=2)` for struct construction.");
    }

    HIRExpr *analyzeBraceInit(AstBraceInit *node, TypeClass *expectedType) {
        if (!expectedType) {
            error(node->loc,
                  "brace initializer needs an explicit target type",
                  "Use it in a typed variable declaration like `var matrix i32[4][5] = {{}}`, or pass it to a call target that already expects an array type.");
        }
        auto initList = buildInitialList(node);
        return materializeInitList(*initList, expectedType, node->loc);
    }

    HIRExpr *analyzeField(AstField *node) {
        auto *binding = resolved.field(node);
        if (!binding) {
            internalError(node->loc,
                          "missing resolved identifier binding for `" +
                              toStdString(node->name) + "`",
                          "Run name resolution before HIR lowering.");
        }

        if (binding->kind() == ResolvedValueRef::Kind::GlobalObject) {
            auto *obj = requireGlobalObject(binding->globalName(), node->loc,
                                            "global identifier");
            return makeHIR<HIRValue>(obj, node->loc);
        }
        if (binding->kind() == ResolvedValueRef::Kind::GlobalType) {
            auto *type =
                requireTypeByName(binding->globalName(), node->loc, "type identifier");
            return makeHIR<HIRValue>(new TypeObject(type), node->loc);
        }

        return makeHIR<HIRValue>(
            requireBoundObject(binding->localBinding(), node->loc), node->loc);
    }

    HIRExpr *analyzeFuncRef(AstFuncRef *node) {
        auto *functionName = resolved.functionRef(node);
        if (!functionName) {
            internalError(node->loc,
                          "missing resolved function reference for `" +
                              toStdString(node->name) + "`",
                          "Run name resolution before HIR lowering.");
        }

        auto *func = requireGlobalFunction(*functionName, node->loc,
                                           "function reference");
        auto *funcType = func->getType() ? func->getType()->as<FuncType>() : nullptr;
        if (!funcType) {
            internalError(node->loc,
                          "invalid resolved function reference target `" +
                              toStdString(node->name) + "`",
                          "This looks like a compiler pipeline bug.");
        }

        const auto &expectedArgTypes = funcType->getArgTypes();
        const auto actualArgCount = node->argTypes ? node->argTypes->size() : 0;
        if (expectedArgTypes.size() != actualArgCount) {
            error("function reference parameter count mismatch for `" +
                  toStdString(node->name) + "`: expected " +
                  std::to_string(expectedArgTypes.size()) + ", got " +
                  std::to_string(actualArgCount));
        }

        for (size_t i = 0; i < actualArgCount; ++i) {
            auto actualBindingKind = funcParamBindingKind(node->argTypes->at(i));
            auto *actualType = requireType(
                unwrapFuncParamType(node->argTypes->at(i)),
                "unknown function reference parameter type at index " +
                    std::to_string(i) + " for `" + toStdString(node->name) +
                    "`: " + describeTypeNode(node->argTypes->at(i)));
            auto *expectedType = expectedArgTypes[i];
            auto expectedBindingKind = funcType->getArgBindingKind(i);
            if (actualBindingKind != expectedBindingKind || actualType != expectedType) {
                auto expectedDescription =
                    (expectedBindingKind == BindingKind::Ref ? "ref " : "") +
                    describeResolvedType(expectedType);
                auto actualDescription =
                    (actualBindingKind == BindingKind::Ref ? "ref " : "") +
                    describeResolvedType(actualType);
                error("function reference parameter type mismatch at index " +
                      std::to_string(i) + " for `" + toStdString(node->name) +
                      "`: expected " + expectedDescription + ", got " +
                      actualDescription);
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
                  "You can assign to variables, struct fields, dereferenced pointers, or array indexing expressions.");
        }
        auto *right = requireNonCallExpr(node->right, left ? left->getType() : nullptr);
        right = coerceNumericExpr(right, left ? left->getType() : nullptr, node->loc,
                                  false);
        auto *leftType = left->getType();
        auto *rightType = right->getType();
        if (!leftType || !rightType || !isByteCopyCompatible(leftType, rightType)) {
            requireCompatibleTypes(node->loc, leftType, rightType,
                                   "assignment type mismatch");
        }
        return makeHIR<HIRAssign>(left, right, node->loc);
    }

    HIRExpr *analyzeBinOper(AstBinOper *node, TypeClass *expectedType = nullptr) {
        TypeClass *contextualOperandType =
            expectedType && isNumericType(expectedType) ? expectedType : nullptr;
        auto *left = requireNonCallExpr(node->left, contextualOperandType);
        auto *right = requireNonCallExpr(
            node->right, contextualOperandType ? contextualOperandType
                                               : (left ? left->getType() : nullptr));
        if (left->getType() != right->getType()) {
            auto *leftConst = node->left ? node->left->as<AstConst>() : nullptr;
            if (leftConst && leftConst->getType() == AstConst::Type::FP64 &&
                isFloatType(right->getType())) {
                left = requireNonCallExpr(node->left, right->getType());
            }
        }
        if (left->getType() != right->getType()) {
            auto *rightConst = node->right ? node->right->as<AstConst>() : nullptr;
            if (rightConst && rightConst->getType() == AstConst::Type::FP64 &&
                isFloatType(left->getType())) {
                right = requireNonCallExpr(node->right, left->getType());
            }
        }
        if (left->getType() != right->getType()) {
            if (auto *commonType = commonNumericType(left->getType(), right->getType())) {
                left = coerceNumericExpr(left, commonType, node->left->loc, false);
                right = coerceNumericExpr(right, commonType, node->right->loc, false);
            }
        }
        auto binding =
            operatorResolver.resolveBinary(node->op, left->getType(),
                                           right->getType(), node->loc);
        return makeHIR<HIRBinOper>(binding, left, right, binding.resultType,
                                   node->loc);
    }

    HIRExpr *analyzeUnaryOper(AstUnaryOper *node, TypeClass *expectedType = nullptr) {
        TypeClass *contextualOperandType =
            expectedType && isNumericType(expectedType) ? expectedType : nullptr;
        auto *value = requireNonCallExpr(node->expr, contextualOperandType);
        auto binding =
            operatorResolver.resolveUnary(node->op, value->getType(),
                                          isAddressable(value), node->loc);
        return makeHIR<HIRUnaryOper>(binding, value, binding.resultType, node->loc);
    }

    HIRNode *analyzeVarDef(AstVarDef *node) {
        if (auto *typeNode = node ? node->getTypeNode() : nullptr) {
            validateTypeNodeLayout(typeNode);
        }
        rejectUninitializedFunctionDerivedStorage(node);
        const bool isRefBinding = node->isRefBinding();

        TypeClass *type = nullptr;
        if (auto *typeNode = node->getTypeNode()) {
            type = requireType(typeNode, "unknown variable type");
            rejectBareFunctionStorage(type, node);
        } else if (isRefBinding) {
            error(node->loc,
                  "reference binding `" + toStdString(node->getName()) +
                      "` requires an explicit type annotation",
                  "Write `ref name Type = value` so the alias target type is explicit.");
        }

        HIRExpr *init = nullptr;
        if (node->withInitVal()) {
            init = isRefBinding
                ? requireNonCallExpr(node->getInitVal(), type)
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
                        error(node->getInitVal() ? node->getInitVal()->loc : node->loc,
                              "reference binding expects an addressable value",
                              "Bind references to variables, struct fields, dereferenced pointers, or array indexing expressions.");
                    }
                    if (type != init->getType()) {
                        error(node->loc,
                              "reference binding type mismatch for `" +
                                  toStdString(node->getName()) + "`: expected " +
                                  describeResolvedType(type) + ", got " +
                                  describeResolvedType(init->getType()),
                              "References bind aliases directly, so the referenced value must have the exact declared type.");
                    }
                } else {
                    init = coerceNumericExpr(init, type, node->loc, false);
                    requireCompatibleTypes(node->loc, type, init->getType(),
                                           "initializer type mismatch for `" +
                                               toStdString(node->getName()) + "`");
                }
            }
        } else if (init) {
            rejectMethodSelectorStorage(typeMgr, init, node);
            type = init->getType();
            if (!type) {
                auto *value = dynamic_cast<HIRValue *>(init);
                auto *object = value ? value->getValue() : nullptr;
                if (object && object->as<ModuleObject>()) {
                    error(node->loc,
                          "module namespaces can't be stored as runtime values",
                          "Access a concrete member like `file.func(...)` instead.");
                }
                if (object && object->as<TypeObject>()) {
                    error(node->loc,
                          "type names can't be stored as runtime values",
                          "Call the type like `Vec2(...)`, or use it in a type annotation.");
                }
                error(node->loc,
                      "this expression doesn't produce a storable runtime value");
            }
            rejectBareFunctionStorage(type, node);
        } else {
            error(node->loc,
                  "cannot infer the type of `" + toStdString(node->getName()) +
                      "` without an initializer",
                  "Add an explicit type annotation or provide an initializer.");
        }

        auto *binding = resolved.variable(node);
        if (!binding) {
            internalError(node->loc,
                          "missing resolved variable binding for `" +
                              toStdString(node->getName()) + "`",
                          "Run name resolution before HIR lowering.");
        }
        auto *obj = type->newObj(Object::VARIABLE |
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
                error("unexpected return value in void function");
            }
            expr = coerceNumericExpr(expr, retType, node->expr->loc, false);
            requireCompatibleTypes(node->loc, retType, expr->getType(),
                                   "return type mismatch");
        } else if (retType) {
            error("missing return value");
        }
        return makeHIR<HIRRet>(expr, node->loc);
    }

    HIRNode *analyzeIf(AstIf *node) {
        auto *cond = requireNonCallExpr(node->condition);
        if (!isTruthyScalarType(cond->getType())) {
            error(node->condition ? node->condition->loc : node->loc,
                  "if condition expects a scalar truthy value",
                  "Use `bool`, numeric values, or pointers in condition expressions.");
        }
        auto *thenBlock = analyzeBlock(node->then);
        auto *elseBlock = node->hasElse() ? analyzeBlock(node->els) : nullptr;
        return makeHIR<HIRIf>(cond, thenBlock, elseBlock, node->loc);
    }

    HIRNode *analyzeFor(AstFor *node) {
        auto *cond = requireNonCallExpr(node->expr);
        if (!isTruthyScalarType(cond->getType())) {
            error(node->expr ? node->expr->loc : node->loc,
                  "for condition expects a scalar truthy value",
                  "Use `bool`, numeric values, or pointers in loop conditions.");
        }
        auto *body = analyzeBlock(node->body);
        return makeHIR<HIRFor>(cond, body, node->loc);
    }

    HIRExpr *analyzeSelector(AstSelector *node) {
        auto *parent = requireExpr(node->parent);
        if (auto *parentValue = dynamic_cast<HIRValue *>(parent)) {
            if (auto *moduleObject = parentValue->getValue()->as<ModuleObject>()) {
                auto fieldName = toStdString(node->field->text);
                const auto *typeName = moduleObject->unit()
                    ? moduleObject->unit()->findLocalType(fieldName)
                    : nullptr;
                if (typeName) {
                    auto *type = requireTypeByName(*typeName, node->loc, "module type");
                    return makeHIR<HIRValue>(new TypeObject(type), node->loc);
                }
                const auto *functionName = moduleObject->unit()
                    ? moduleObject->unit()->findLocalFunction(fieldName)
                    : nullptr;
                if (!functionName) {
                    auto moduleName = moduleObject->unit()
                        ? moduleObject->unit()->moduleName()
                        : std::string("<module>");
                    error(node->loc,
                          "unknown module member `" + moduleName + "." + fieldName + "`",
                          "Only directly imported top-level functions and types are available through `file.xxx`.");
                }
                auto *func = requireGlobalFunction(*functionName, node->loc,
                                                  "module function");
                return makeHIR<HIRValue>(func, node->loc);
            }
        }
        auto fieldName = toStdString(node->field->text);
        if (resolveInjectedMemberBinding(typeMgr, parent, fieldName)) {
            error(node->loc,
                  "injected member `" + fieldName +
                      "` can only be used as a direct call callee",
                  describeInjectedMemberHelp(parent->getType(), fieldName));
        }
        auto *tupleType = parent->getType() ? parent->getType()->as<TupleType>() : nullptr;
        if (tupleType) {
            TupleType::ValueTy member;
            if (!tupleType->getMember(llvm::StringRef(fieldName), member)) {
                error(node->loc,
                      "unknown tuple field `" + fieldName + "`",
                      describeTupleFieldHelp(tupleType));
            }
            return makeHIR<HIRSelector>(parent, fieldName, member.first, node->loc);
        }
        auto *structType = parent->getType() ? parent->getType()->as<StructType>() : nullptr;
        if (!structType) {
            error(node->loc,
                  "member access expects a struct or tuple value on the left side");
        }

        auto *member = structType->getMember(llvm::StringRef(fieldName));
        if (member) {
            return makeHIR<HIRSelector>(parent, fieldName, member->first, node->loc);
        }
        if (structType->getMethodType(llvm::StringRef(fieldName))) {
            return makeHIR<HIRSelector>(parent, fieldName, nullptr, node->loc);
        }
        error(node->loc, "unknown struct field `" + fieldName + "`",
              "Check the field name, or use a direct method call like `obj.method(...)`.");
    }

    HIRExpr *analyzeCall(AstFieldCall *node, TypeClass *expectedType = nullptr) {
        if (auto *selectorNode = node->value ? node->value->as<AstSelector>() : nullptr) {
            auto *receiver = requireExpr(selectorNode->parent);
            auto fieldName = toStdString(selectorNode->field->text);
            if (auto binding = resolveInjectedMemberBinding(typeMgr, receiver, fieldName)) {
                if (binding->kind == InjectedMemberKind::NumericConversion) {
                    if (node->args && !node->args->empty()) {
                        error(node->loc,
                              "numeric conversion member `" + fieldName +
                                  "` does not take arguments",
                              "Call it as `<expr>." + fieldName + "()`.");
                    }
                    return coerceNumericExpr(receiver, binding->resultType, node->loc,
                                             true);
                }
                if (binding->kind == InjectedMemberKind::BitCopy) {
                    if (node->args && !node->args->empty()) {
                        error(node->loc,
                              "raw bit-copy member `" + fieldName +
                                  "` does not take arguments",
                              "Call it as `<expr>." + fieldName + "()`.");
                    }
                    return coerceBitCopyExpr(receiver, binding->resultType, node->loc);
                }
            }
        }

        auto *callee = requireExpr(node->value);
        if (auto *calleeValue = dynamic_cast<HIRValue *>(callee)) {
            if (auto *typeObject = calleeValue->getValue()->as<TypeObject>()) {
                auto *structType = typeObject->declaredType()
                    ? typeObject->declaredType()->as<StructType>()
                    : nullptr;
                if (!structType) {
                    error(node->loc,
                          "constructor calls currently support struct types only",
                          "Use a struct type like `Vec2(...)`. Numeric conversion still uses `.toXXX()`.");
                }

                auto members = orderedStructMembers(structType, node->loc);
                std::vector<std::string> fieldNames;
                fieldNames.reserve(members.size());
                for (const auto &member : members) {
                    fieldNames.push_back(member.first);
                }
                auto orderedArgs = collectOrderedCallArgs(
                    node->args, fieldNames, node->loc,
                    "constructor `" + describeResolvedType(structType) + "`",
                    "field", true);
                if (orderedArgs.size() != members.size()) {
                    error(node->loc,
                          "constructor argument count mismatch for `" +
                              describeResolvedType(structType) + "`: expected " +
                              std::to_string(members.size()) + ", got " +
                              std::to_string(orderedArgs.size()));
                }

                std::vector<HIRExpr *> fields;
                fields.reserve(orderedArgs.size());
                for (size_t i = 0; i < orderedArgs.size(); ++i) {
                    if (orderedArgs[i].bindingKind == BindingKind::Ref) {
                        error(orderedArgs[i].syntax ? orderedArgs[i].syntax->loc
                                                    : node->loc,
                              "constructor arguments do not accept `ref`",
                              "Constructors copy field values. Remove `ref` from this argument.");
                    }
                    auto *expectedFieldType = members[i].second;
                    auto *fieldExpr =
                        requireNonCallExpr(orderedArgs[i].value, expectedFieldType);
                    fieldExpr = coerceNumericExpr(fieldExpr, expectedFieldType,
                                                 orderedArgs[i].value->loc, false);
                    requireCompatibleTypes(
                        orderedArgs[i].value->loc, expectedFieldType, fieldExpr->getType(),
                        "constructor field type mismatch for `" + members[i].first + "`");
                    fields.push_back(fieldExpr);
                }
                return makeHIR<HIRStructLiteral>(std::move(fields), structType,
                                                 node->loc);
            }
        }

        FuncType *funcType = nullptr;
        size_t argOffset = 0;
        const std::vector<std::string> *paramNames = nullptr;
        if (auto *selector = dynamic_cast<HIRSelector *>(callee);
            selector && selector->getType() == nullptr) {
            auto *structType = selector->getParent()->getType()->as<StructType>();
            if (!structType) {
                error("selector call parent must be a struct value");
            }
            auto *methodFunc =
                typeMgr->getMethodFunction(structType,
                                           llvm::StringRef(selector->getFieldName()));
            funcType =
                methodFunc ? methodFunc->getType()->as<FuncType>()
                           : structType->getMethodType(
                                 llvm::StringRef(selector->getFieldName()));
            if (!funcType) {
                error("unknown struct method");
            }
            if (methodFunc && !methodFunc->paramNames().empty()) {
                paramNames = &methodFunc->paramNames();
            } else if (const auto *methodParamNames =
                           structType->getMethodParamNames(
                               llvm::StringRef(selector->getFieldName()));
                       methodParamNames && !methodParamNames->empty()) {
                paramNames = methodParamNames;
            }
            argOffset = getMethodCallArgOffset(selector, funcType);
        } else if (auto *func = getDirectFunctionCallee(callee)) {
            funcType = func->getType()->as<FuncType>();
            if (!func->paramNames().empty()) {
                paramNames = &func->paramNames();
            }
        } else if (auto *pointerTarget = getFunctionPointerTarget(callee->getType())) {
            funcType = pointerTarget;
        } else if (auto *arrayType = callee->getType() ? callee->getType()->as<ArrayType>() : nullptr) {
            auto orderedArgs = collectOrderedCallArgs(
                node->args, {}, node->loc, "array indexing", "index", false);
            if (!arrayType->hasStaticLayout()) {
                error(node->loc,
                      "array indexing requires fixed explicit dimensions",
                      "Use positive integer literal dimensions. Dimension inference and dynamic sizes are not implemented yet.");
            }
            const auto actualArgCount = orderedArgs.size();
            if (actualArgCount != arrayType->indexArity()) {
                error(node->loc,
                      "array index arity mismatch: expected " +
                          std::to_string(arrayType->indexArity()) + ", got " +
                          std::to_string(actualArgCount));
            }
            std::vector<HIRExpr *> args;
            args.reserve(actualArgCount);
            for (size_t i = 0; i < orderedArgs.size(); ++i) {
                if (orderedArgs[i].bindingKind == BindingKind::Ref) {
                    error(orderedArgs[i].syntax ? orderedArgs[i].syntax->loc
                                                : node->loc,
                          "array indexing does not accept `ref` arguments",
                          "Use positional indices like `a(i, j)` without `ref`.");
                }
                auto *arg = requireNonCallExpr(orderedArgs[i].value, i32Ty);
                arg = coerceNumericExpr(arg, i32Ty, orderedArgs[i].value->loc, false);
                requireCompatibleTypes(orderedArgs[i].value->loc, i32Ty, arg->getType(),
                                           "array index type mismatch at index " +
                                               std::to_string(i));
                args.push_back(arg);
            }
            return makeHIR<HIRIndex>(callee, std::move(args),
                                     arrayType->getElementType(), node->loc);
        } else {
            error("callee must be a function, function pointer, or method selector");
        }

        std::vector<CallArgSpec> orderedArgs;
        if (node->args && !node->args->empty()) {
            if (paramNames && !paramNames->empty()) {
                orderedArgs = collectOrderedCallArgs(
                    node->args, *paramNames, node->loc, "function call",
                    "parameter", true);
            } else {
                orderedArgs = collectOrderedCallArgs(
                    node->args, {}, node->loc, "this call target",
                    "parameter", false);
            }
        }

        const auto actualArgCount = orderedArgs.size();
        std::vector<HIRExpr *> args;
        args.reserve(actualArgCount);
        const auto &paramTypes = funcType->getArgTypes();
        if (actualArgCount + argOffset != paramTypes.size()) {
            error("call argument count mismatch: expected " +
                  std::to_string(paramTypes.size() - argOffset) + ", got " +
                  std::to_string(actualArgCount));
        }
        if (!orderedArgs.empty()) {
            for (size_t i = 0; i < orderedArgs.size(); ++i) {
                auto *expectedType = paramTypes[i + argOffset];
                auto bindingKind = funcType->getArgBindingKind(i + argOffset);
                if (bindingKind == BindingKind::Ref) {
                    if (orderedArgs[i].bindingKind != BindingKind::Ref) {
                        error(orderedArgs[i].syntax ? orderedArgs[i].syntax->loc
                                                    : node->loc,
                              "reference " +
                                  describeCallParameter(paramNames, i) +
                                  " must be passed with `ref`",
                              explicitRefCallHint(paramNames, i));
                    }
                    auto *arg =
                        requireNonCallExpr(orderedArgs[i].value, expectedType);
                    if (!isAddressable(arg)) {
                        error(orderedArgs[i].value->loc,
                              "reference parameter at index " + std::to_string(i) +
                                  " expects an addressable value",
                              "Pass a variable, struct field, dereferenced pointer, or array indexing expression.");
                    }
                    if (expectedType != arg->getType()) {
                        error(orderedArgs[i].value->loc,
                              "reference argument type mismatch at index " +
                                  std::to_string(i) + ": expected " +
                                  describeResolvedType(expectedType) + ", got " +
                                  describeResolvedType(arg->getType()),
                              "Reference arguments bind aliases directly, so the argument type must match exactly.");
                    }
                    args.push_back(arg);
                } else {
                    if (orderedArgs[i].bindingKind == BindingKind::Ref) {
                        error(orderedArgs[i].syntax ? orderedArgs[i].syntax->loc
                                                    : node->loc,
                              "value " + describeCallParameter(paramNames, i) +
                                  " cannot be passed with `ref`",
                              "Remove `ref` and pass the value directly.");
                    }
                    auto *arg =
                        requireNonCallExpr(orderedArgs[i].value, expectedType);
                    arg = coerceNumericExpr(arg, expectedType,
                                            orderedArgs[i].value->loc, false);
                    requireCompatibleTypes(orderedArgs[i].value->loc, expectedType,
                                           arg->getType(),
                                           "call argument type mismatch at index " +
                                               std::to_string(i));
                    args.push_back(arg);
                }
            }
        }
        for (size_t i = 0; i < args.size(); ++i) {
            auto *expectedType = paramTypes[i + argOffset];
            auto *actualType = args[i]->getType();
            if (funcType->getArgBindingKind(i + argOffset) == BindingKind::Ref) {
                if (expectedType != actualType) {
                    error(node->loc,
                          "reference argument type mismatch at index " +
                              std::to_string(i) + ": expected " +
                              describeResolvedType(expectedType) + ", got " +
                              describeResolvedType(actualType));
                }
            } else {
                requireCompatibleTypes(node->loc, expectedType, actualType,
                                       "call argument type mismatch at index " +
                                           std::to_string(i));
            }
        }

        auto *retType = funcType ? funcType->getRetType() : nullptr;
        return makeHIR<HIRCall>(callee, std::move(args), retType, node->loc);
    }

public:
    FunctionAnalyzer(TypeTable *typeMgr, GlobalScope *global,
                     HIRModule *ownerModule,
                     const CompilationUnit *unit,
                     const ResolvedFunction &resolved)
        : typeMgr(typeMgr),
          global(global),
          unit(unit),
          resolved(resolved),
          operatorResolver(typeMgr),
          ownerModule(ownerModule),
          hirFunc(nullptr) {
        if (resolved.isTopLevelEntry()) {
            hirFunc = makeHIR<HIRFunc>(getOrCreateTopLevelEntry(global, typeMgr),
                                       getOrCreateMainType(typeMgr), resolved.loc(),
                                       true, resolved.guaranteedReturn());
        } else {
            auto *lofunc = requireDeclaredFunction(resolved.loc());
            auto *funcType = lofunc->getType() ? lofunc->getType()->as<FuncType>() : nullptr;
            if (!funcType) {
                internalError(resolved.loc(),
                              "resolved function type is invalid",
                              "This looks like a compiler pipeline bug.");
            }
            hirFunc = makeHIR<HIRFunc>(
                llvm::cast<llvm::Function>(lofunc->getllvmValue()), funcType,
                resolved.loc(), false, resolved.guaranteedReturn());
        }

        if (resolved.hasSelfBinding()) {
            if (!resolved.isMethod()) {
                internalError(resolved.loc(),
                              "resolved self binding is missing its method parent",
                              "This looks like a compiler pipeline bug.");
            }
            auto *methodParent = requireStructTypeByName(
                resolved.methodParentTypeName(), resolved.loc(), "method parent type");
            auto *selfObj = methodParent->newObj(Object::VARIABLE | Object::REF_ALIAS);
            bindObject(resolved.selfBinding(), selfObj);
            hirFunc->setSelfBinding(
                {resolved.selfBinding()->name(), resolved.selfBinding()->bindingKind(),
                 selfObj, resolved.selfBinding()->loc()});
        }

        for (auto *paramBinding : resolved.params()) {
            auto *decl = paramBinding ? paramBinding->parameterDecl() : nullptr;
            if (!decl) {
                internalError(resolved.loc(),
                              "resolved parameter binding is missing its declaration",
                              "This looks like a compiler pipeline bug.");
            }
            auto *type = requireType(
                decl->typeNode,
                "unknown function argument type for `" + paramBinding->name() + "`");
            auto *argObj = type->newObj(Object::VARIABLE |
                                        (paramBinding->isRefBinding()
                                             ? Object::REF_ALIAS
                                             : Object::EMPTY));
            bindObject(paramBinding, argObj);
            hirFunc->addParam({paramBinding->name(), paramBinding->bindingKind(),
                               argObj, paramBinding->loc()});
        }

        hirFunc->setBody(analyzeBlock(const_cast<AstNode *>(resolved.body())));
    }

    HIRFunc *getFunction() const { return hirFunc; }
};

class ModuleAnalyzer {
    GlobalScope *global;
    TypeTable *typeMgr;
    const CompilationUnit *unit;
    std::unique_ptr<HIRModule> module = std::make_unique<HIRModule>();

public:
    explicit ModuleAnalyzer(GlobalScope *global, const CompilationUnit *unit)
        : global(global), typeMgr(requireTypeTable(global)), unit(unit) {}

    std::unique_ptr<HIRModule>
    analyze(const ResolvedModule &resolvedModule) {
        for (const auto &resolvedFunction : resolvedModule.functions()) {
            module->addFunction(
                FunctionAnalyzer(typeMgr, global, module.get(), unit, *resolvedFunction)
                    .getFunction());
        }
        return std::move(module);
    }
};

}  // namespace

std::unique_ptr<HIRModule>
analyzeModule(GlobalScope *global, const ResolvedModule &resolved,
              const CompilationUnit *unit) {
    return ModuleAnalyzer(global, unit).analyze(resolved);
}

}  // namespace lona
