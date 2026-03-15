#include "lona/sema/hir.hh"
#include "lona/ast/astnode.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sym/func.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include "parser.hh"
#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
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
describeTypeNode(TypeNode *node) {
    if (node == nullptr) {
        return "<unknown type>";
    }
    if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
        return toStdString(base->name);
    }
    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        auto name = describeTypeNode(pointer->base);
        for (uint32_t i = 0; i < pointer->dim; ++i) {
            name += "*";
        }
        return name;
    }
    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        auto name = describeTypeNode(array->base);
        name += "[";
        for (size_t i = 0; i < array->dim.size(); ++i) {
            if (i != 0) {
                name += ",";
            }
            if (array->dim[i] != nullptr) {
                name += "?";
            }
        }
        name += "]";
        return name;
    }
    if (auto *func = dynamic_cast<FuncTypeNode *>(node)) {
        std::string name = "(";
        for (size_t i = 0; i < func->args.size(); ++i) {
            if (i != 0) {
                name += ", ";
            }
            name += describeTypeNode(func->args[i]);
        }
        name += ")";
        if (func->ret) {
            name += " ";
            name += describeTypeNode(func->ret);
        }
        return name;
    }
    return "<unknown type>";
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
        return describeResolvedType(pointer->getPointeeType()) + "*";
    }
    if (auto *array = type->as<ArrayType>()) {
        auto name = describeResolvedType(array->getElementType());
        name += "[";
        const auto &dimensions = array->getDimensions();
        for (size_t i = 0; i < dimensions.size(); ++i) {
            if (i != 0) {
                name += ",";
            }
            if (dimensions[i] != nullptr) {
                name += "?";
            }
        }
        name += "]";
        return name;
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

FuncType *
getMethodSelectorType(HIRSelector *selector) {
    if (!selector || selector->getType() != nullptr) {
        return nullptr;
    }

    auto *parentType = selector->getParent() ? selector->getParent()->getType() : nullptr;
    auto *structType = parentType ? parentType->as<StructType>() : nullptr;
    if (!structType) {
        return nullptr;
    }

    auto *func = structType->getFunc(llvm::StringRef(selector->getFieldName()));
    if (!func) {
        return nullptr;
    }
    return func->getType() ? func->getType()->as<FuncType>() : nullptr;
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
rejectMethodSelectorStorage(HIRExpr *expr, AstVarDef *node) {
    auto *selector = dynamic_cast<HIRSelector *>(expr);
    auto *funcType = getMethodSelectorType(selector);
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
rejectNonCallMethodSelector(HIRExpr *expr) {
    auto *selector = dynamic_cast<HIRSelector *>(expr);
    if (!selector || !getMethodSelectorType(selector)) {
        return;
    }

    error(selector->getLocation(), kMethodSelectorDirectCallError,
          "Call the method directly as `obj.method(...)`.");
}

void
rejectBareFunctionStorage(TypeClass *type, AstVarDef *node) {
    if (!node) {
        return;
    }
    bool hasBareFunctionStorage = type && type->as<FuncType>();
    if (!hasBareFunctionStorage) {
        auto *typeNode = node->getTypeNode();
        hasBareFunctionStorage =
            typeNode != nullptr && dynamic_cast<FuncTypeNode *>(typeNode) != nullptr;
    }
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
    if (!typeNode || dynamic_cast<FuncTypeNode *>(typeNode) != nullptr) {
        return;
    }
    if (findFuncTypeNode(typeNode) == nullptr) {
        return;
    }

    error(node->loc,
          "function-related variable type for `" +
              toStdString(node->getName()) + "` requires initializer: " +
              describeTypeNode(typeNode),
          "Initialize function pointers and function-related arrays at the point of definition.");
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
    if (auto *existing = typeMgr->getType(llvm::StringRef(mainFunctionTypeName()))) {
        return existing->as<FuncType>();
    }

    auto *llvmMainType = llvm::FunctionType::get(i32Ty->getLLVMType(), false);
    auto typeName = mainFunctionTypeName();
    auto *mainType = new FuncType(llvmMainType, {}, i32Ty, string(typeName.c_str()), 0);
    typeMgr->addType(llvm::StringRef(typeName), mainType);
    return mainType;
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
        llvm::cast<llvm::FunctionType>(mainType->getLLVMType()),
        llvm::Function::ExternalLinkage, llvm::Twine(entryName), global->module);
}

class FunctionAnalyzer {
    TypeTable *typeMgr;
    GlobalScope *global;
    const CompilationUnit *unit;
    const ResolvedFunction &resolved;
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
        auto *type = unit ? unit->resolveType(typeMgr, node) : typeMgr->getType(node);
        if (!type) {
            error(currentLocation, context);
        }
        return type;
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

    Function *requireDeclaredFunction(const location &loc) {
        if (!resolved.hasDeclaredFunction()) {
            internalError(loc,
                          "resolved function is missing its stable symbol identity",
                          "This looks like a compiler pipeline bug.");
        }
        if (resolved.isMethod()) {
            auto *structType = requireStructTypeByName(
                resolved.methodParentTypeName(), loc, "method parent type");
            auto *func = structType->getFunc(llvm::StringRef(resolved.functionName()));
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

    HIRExpr *requireExpr(AstNode *node) {
        auto *expr = analyzeExpr(node);
        if (!expr) {
            error(node ? node->loc : currentLocation,
                  "expression did not produce a value");
        }
        return expr;
    }

    HIRExpr *requireNonCallExpr(AstNode *node) {
        auto *expr = requireExpr(node);
        rejectNonCallMethodSelector(expr);
        return expr;
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

    HIRExpr *analyzeExpr(AstNode *node) {
        ScopedLocation scopedLocation(*this, node ? node->loc : location());
        if (!node) {
            return nullptr;
        }
        if (auto *constant = node->as<AstConst>()) {
            return analyzeConst(constant);
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
            return analyzeBinOper(bin);
        }
        if (auto *unary = node->as<AstUnaryOper>()) {
            return analyzeUnaryOper(unary);
        }
        if (auto *selector = node->as<AstSelector>()) {
            return analyzeSelector(selector);
        }
        if (auto *call = node->as<AstFieldCall>()) {
            return analyzeCall(call);
        }
        error("unsupported AST node in HIR analysis");
    }

    HIRExpr *analyzeConst(AstConst *node) {
        switch (node->getType()) {
        case AstConst::Type::INT32:
            return makeHIR<HIRValue>(new ConstVar(i32Ty, *node->getBuf<int32_t>()),
                                     node->loc);
        case AstConst::Type::BOOL:
            return makeHIR<HIRValue>(new ConstVar(boolTy, *node->getBuf<bool>()),
                                     node->loc);
        default:
            error("only i32 and bool constants are supported");
        }
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
            auto *actualType = requireType(
                node->argTypes->at(i),
                "unknown function reference parameter type at index " +
                    std::to_string(i) + " for `" + toStdString(node->name) +
                    "`: " + describeTypeNode(node->argTypes->at(i)));
            auto *expectedType = expectedArgTypes[i];
            if (actualType != expectedType) {
                error("function reference parameter type mismatch at index " +
                      std::to_string(i) + " for `" + toStdString(node->name) +
                      "`: expected " + describeResolvedType(expectedType) +
                      ", got " + describeResolvedType(actualType));
            }
        }

        auto *pointerType = typeMgr->createPointerType(funcType);
        auto *value = pointerType->newObj(Object::REG_VAL | Object::READONLY);
        value->bindllvmValue(func->getllvmValue());
        return makeHIR<HIRValue>(value, node->loc);
    }

    HIRExpr *analyzeAssign(AstAssign *node) {
        auto *left = requireNonCallExpr(node->left);
        auto *right = requireNonCallExpr(node->right);
        auto *leftType = left->getType();
        auto *rightType = right->getType();
        if (!leftType || !rightType || leftType != rightType) {
            error(node->loc,
                  "assignment type mismatch: expected " +
                      describeResolvedType(leftType) + ", got " +
                      describeResolvedType(rightType));
        }
        return makeHIR<HIRAssign>(left, right, node->loc);
    }

    HIRExpr *analyzeBinOper(AstBinOper *node) {
        auto *left = requireNonCallExpr(node->left);
        auto *right = requireNonCallExpr(node->right);
        if (left->getType() != right->getType()) {
            error(node->loc,
                  "binary operation type mismatch: left side is " +
                      describeResolvedType(left->getType()) + ", right side is " +
                      describeResolvedType(right->getType()));
        }
        if (left->getType() != i32Ty) {
            error(node->loc, "binary operations currently only support i32 operands");
        }

        TypeClass *resultType = i32Ty;
        switch (node->op) {
        case '+':
        case '-':
        case '*':
        case '/':
            break;
        case '<':
        case '>':
        case Parser::token::LOGIC_EQUAL:
        case Parser::token::LOGIC_NOT_EQUAL:
            resultType = boolTy;
            break;
        default:
            error("unsupported binary operator");
        }

        return makeHIR<HIRBinOper>(node->op, left, right, resultType, node->loc);
    }

    HIRExpr *analyzeUnaryOper(AstUnaryOper *node) {
        auto *value = requireNonCallExpr(node->expr);
        switch (node->op) {
        case '+':
        case '-':
            if (value->getType() != i32Ty) {
                error(node->op == '+' ? "unary + expects i32" : "unary - expects i32");
            }
            return makeHIR<HIRUnaryOper>(node->op, value, i32Ty, node->loc);
        case '!':
            return makeHIR<HIRUnaryOper>(node->op, value, boolTy, node->loc);
        case '&':
            if (!isAddressable(value)) {
                error("address-of expects an addressable value");
            }
            return makeHIR<HIRUnaryOper>(
                node->op, value, typeMgr->createPointerType(value->getType()), node->loc);
        case '*': {
            auto *pointerType = value->getType() ? value->getType()->as<PointerType>() : nullptr;
            if (!pointerType) {
                error("dereference expects a pointer value");
            }
            return makeHIR<HIRUnaryOper>(node->op, value,
                                         pointerType->getPointeeType(), node->loc);
        }
        default:
            return makeHIR<HIRUnaryOper>(node->op, value, value->getType(),
                                         node->loc);
        }
    }

    HIRNode *analyzeVarDef(AstVarDef *node) {
        HIRExpr *init = nullptr;
        if (node->withInitVal()) {
            init = requireExpr(node->getInitVal());
        }
        rejectUninitializedFunctionDerivedStorage(node);

        TypeClass *type = nullptr;
        if (auto *typeNode = node->getTypeNode()) {
            type = requireType(typeNode, "unknown variable type");
            rejectBareFunctionStorage(type, node);
            if (init && init->getType() != type) {
                error(node->loc,
                      "initializer type mismatch for `" +
                          toStdString(node->getName()) + "`: expected " +
                          describeResolvedType(type) + ", got " +
                          describeResolvedType(init->getType()));
            }
        } else if (init) {
            rejectMethodSelectorStorage(init, node);
            type = init->getType();
            if (!type) {
                error(node->loc,
                      "module namespaces can't be stored as runtime values",
                      "Access a concrete member like `file.func(...)` instead.");
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
        auto *obj = type->newObj(Object::VARIABLE);
        bindObject(binding, obj);
        return makeHIR<HIRVarDef>(binding->name(), obj, init, node->loc);
    }

    HIRNode *analyzeRet(AstRet *node) {
        auto *retType = hirFunc->getFuncType()->getRetType();
        HIRExpr *expr = nullptr;
        if (node->expr) {
            expr = requireNonCallExpr(node->expr);
            if (!retType) {
                error("unexpected return value in void function");
            }
            if (expr->getType() != retType) {
                error("return type mismatch: expected " +
                      describeResolvedType(retType) + ", got " +
                      describeResolvedType(expr->getType()));
            }
        } else if (retType) {
            error("missing return value");
        }
        return makeHIR<HIRRet>(expr, node->loc);
    }

    HIRNode *analyzeIf(AstIf *node) {
        auto *cond = requireNonCallExpr(node->condition);
        auto *thenBlock = analyzeBlock(node->then);
        auto *elseBlock = node->hasElse() ? analyzeBlock(node->els) : nullptr;
        return makeHIR<HIRIf>(cond, thenBlock, elseBlock, node->loc);
    }

    HIRNode *analyzeFor(AstFor *node) {
        auto *cond = requireNonCallExpr(node->expr);
        auto *body = analyzeBlock(node->body);
        return makeHIR<HIRFor>(cond, body, node->loc);
    }

    HIRExpr *analyzeSelector(AstSelector *node) {
        auto *parent = requireExpr(node->parent);
        if (auto *parentValue = dynamic_cast<HIRValue *>(parent)) {
            if (auto *moduleObject = parentValue->getValue()->as<ModuleObject>()) {
                auto fieldName = toStdString(node->field->text);
                const auto *functionName = moduleObject->unit()
                    ? moduleObject->unit()->findLocalFunction(fieldName)
                    : nullptr;
                if (!functionName) {
                    auto moduleName = moduleObject->unit()
                        ? moduleObject->unit()->moduleName()
                        : std::string("<module>");
                    error(node->loc,
                          "unknown module member `" + moduleName + "." + fieldName + "`",
                          "Only imported top-level functions are available through `file.xxx`.");
                }
                auto *func = requireGlobalFunction(*functionName, node->loc,
                                                  "module function");
                return makeHIR<HIRValue>(func, node->loc);
            }
        }
        auto *structType = parent->getType() ? parent->getType()->as<StructType>() : nullptr;
        if (!structType) {
            error(node->loc, "member access expects a struct value on the left side");
        }

        auto fieldName = toStdString(node->field->text);
        auto *member = structType->getMember(llvm::StringRef(fieldName));
        if (member) {
            return makeHIR<HIRSelector>(parent, fieldName, member->first, node->loc);
        }
        if (structType->getFunc(llvm::StringRef(fieldName))) {
            return makeHIR<HIRSelector>(parent, fieldName, nullptr, node->loc);
        }
        error(node->loc, "unknown struct field `" + fieldName + "`",
              "Check the field name, or use a direct method call like `obj.method(...)`.");
    }

    HIRExpr *analyzeCall(AstFieldCall *node) {
        auto *callee = requireExpr(node->value);
        std::vector<HIRExpr *> args;
        if (node->args) {
            args.reserve(node->args->size());
            for (auto *arg : *node->args) {
                args.push_back(requireNonCallExpr(arg));
            }
        }

        FuncType *funcType = nullptr;
        size_t argOffset = 0;
        if (auto *selector = dynamic_cast<HIRSelector *>(callee);
            selector && selector->getType() == nullptr) {
            auto *structType = selector->getParent()->getType()->as<StructType>();
            if (!structType) {
                error("selector call parent must be a struct value");
            }
            auto *func = structType->getFunc(llvm::StringRef(selector->getFieldName()));
            if (!func) {
                error("unknown struct method");
            }
            funcType = func->getType()->as<FuncType>();
            argOffset = getMethodCallArgOffset(selector, funcType);
        } else if (auto *func = getDirectFunctionCallee(callee)) {
            funcType = func->getType()->as<FuncType>();
        } else if (auto *pointerTarget = getFunctionPointerTarget(callee->getType())) {
            funcType = pointerTarget;
        } else {
            error("callee must be a function, function pointer, or method selector");
        }

        const auto &paramTypes = funcType->getArgTypes();
        if (args.size() + argOffset != paramTypes.size()) {
            error("call argument count mismatch: expected " +
                  std::to_string(paramTypes.size() - argOffset) + ", got " +
                  std::to_string(args.size()));
        }
        for (size_t i = 0; i < args.size(); ++i) {
            auto *expectedType = paramTypes[i + argOffset];
            auto *actualType = args[i]->getType();
            if (actualType != expectedType) {
                error("call argument type mismatch at index " + std::to_string(i) +
                      ": expected " + describeResolvedType(expectedType) +
                      ", got " + describeResolvedType(actualType));
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
            auto *selfObj = methodParent->newObj(Object::VARIABLE);
            bindObject(resolved.selfBinding(), selfObj);
            hirFunc->setSelfBinding(
                {resolved.selfBinding()->name(), selfObj, resolved.selfBinding()->loc()});
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
            auto *argObj = type->newObj(Object::VARIABLE);
            bindObject(paramBinding, argObj);
            hirFunc->addParam({paramBinding->name(), argObj, paramBinding->loc()});
        }

        hirFunc->setBody(analyzeBlock(const_cast<AstNode *>(resolved.body())));
    }

    HIRFunc *getFunction() const { return hirFunc; }
};

class ModuleAnalyzer {
    GlobalScope *global;
    TypeTable *typeMgr;
    const CompilationUnit *unit;
    HIRModule *module = new HIRModule();

public:
    explicit ModuleAnalyzer(GlobalScope *global, const CompilationUnit *unit)
        : global(global), typeMgr(requireTypeTable(global)), unit(unit) {}

    HIRModule *analyze(const ResolvedModule &resolvedModule) {
        for (const auto &resolvedFunction : resolvedModule.functions()) {
            module->addFunction(
                FunctionAnalyzer(typeMgr, global, module, unit, *resolvedFunction)
                    .getFunction());
        }
        return module;
    }
};

}  // namespace

HIRModule *
analyzeModule(GlobalScope *global, const ResolvedModule &resolved,
              const CompilationUnit *unit) {
    return ModuleAnalyzer(global, unit).analyze(resolved);
}

}  // namespace lona
