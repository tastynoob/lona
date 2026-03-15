#include "lona/sema/hir.hh"
#include "lona/ast/astnode.hh"
#include "lona/sym/func.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include "parser.hh"
#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
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
    throw std::runtime_error(message);
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

    error("unsupported bare function variable type for `" +
          toStdString(node->getName()) + "`: " +
          describeMethodSelectorType(selector, funcType));
}

void
rejectNonCallMethodSelector(HIRExpr *expr) {
    auto *selector = dynamic_cast<HIRSelector *>(expr);
    if (!selector || !getMethodSelectorType(selector)) {
        return;
    }

    error(kMethodSelectorDirectCallError);
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
    error("unsupported bare function variable type for `" +
          toStdString(node->getName()) + "`: " +
          describeStorageType(type, node));
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

    error("function-related variable type for `" +
          toStdString(node->getName()) + "` requires initializer: " +
          describeTypeNode(typeNode));
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
    FuncScope *scope;
    StructType *methodParent = nullptr;
    HIRFunc *hirFunc;
    bool skipDeclStatements = false;

    TypeClass *requireType(TypeNode *node, const std::string &context) {
        auto *type = typeMgr->getType(node);
        if (!type) {
            error(context);
        }
        return type;
    }

    HIRExpr *requireExpr(AstNode *node) {
        auto *expr = analyzeExpr(node);
        if (!expr) {
            error("expression did not produce a value");
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
        auto *block = new HIRBlock(node ? node->loc : location());
        if (!node) {
            return block;
        }

        if (auto *list = node->as<AstStatList>()) {
            for (auto *stmt : list->getBody()) {
                if (skipDeclStatements &&
                    (stmt->is<AstStructDecl>() || stmt->is<AstFuncDecl>())) {
                    continue;
                }
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
            return new HIRValue(new ConstVar(i32Ty, *node->getBuf<int32_t>()),
                                node->loc);
        case AstConst::Type::BOOL:
            return new HIRValue(new ConstVar(boolTy, *node->getBuf<bool>()), node->loc);
        default:
            error("only i32 and bool constants are supported");
        }
    }

    HIRExpr *analyzeField(AstField *node) {
        auto *obj = scope->getObj(node->name);
        if (!obj) {
            error("undefined identifier");
        }
        return new HIRValue(obj, node->loc);
    }

    HIRExpr *analyzeFuncRef(AstFuncRef *node) {
        auto *obj = global->getObj(node->name);
        auto *func = obj ? obj->as<Function>() : nullptr;
        if (!func) {
            error("undefined function reference `" + toStdString(node->name) + "`");
        }

        auto *funcType = func->getType() ? func->getType()->as<FuncType>() : nullptr;
        if (!funcType) {
            error("invalid function reference target `" + toStdString(node->name) + "`");
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
        return new HIRValue(value, node->loc);
    }

    HIRExpr *analyzeAssign(AstAssign *node) {
        auto *left = requireNonCallExpr(node->left);
        auto *right = requireNonCallExpr(node->right);
        auto *leftType = left->getType();
        auto *rightType = right->getType();
        if (!leftType || !rightType || leftType != rightType) {
            error("type mismatch");
        }
        return new HIRAssign(left, right, node->loc);
    }

    HIRExpr *analyzeBinOper(AstBinOper *node) {
        auto *left = requireNonCallExpr(node->left);
        auto *right = requireNonCallExpr(node->right);
        if (left->getType() != right->getType()) {
            error("type mismatch in binary operation");
        }
        if (left->getType() != i32Ty) {
            error("only i32 binary operations are supported");
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

        return new HIRBinOper(node->op, left, right, resultType, node->loc);
    }

    HIRExpr *analyzeUnaryOper(AstUnaryOper *node) {
        auto *value = requireNonCallExpr(node->expr);
        switch (node->op) {
        case '+':
        case '-':
            if (value->getType() != i32Ty) {
                error(node->op == '+' ? "unary + expects i32" : "unary - expects i32");
            }
            return new HIRUnaryOper(node->op, value, i32Ty, node->loc);
        case '!':
            return new HIRUnaryOper(node->op, value, boolTy, node->loc);
        case '&':
            if (!isAddressable(value)) {
                error("address-of expects an addressable value");
            }
            return new HIRUnaryOper(
                node->op, value, typeMgr->createPointerType(value->getType()), node->loc);
        case '*': {
            auto *pointerType = value->getType() ? value->getType()->as<PointerType>() : nullptr;
            if (!pointerType) {
                error("dereference expects a pointer value");
            }
            return new HIRUnaryOper(node->op, value, pointerType->getPointeeType(),
                                    node->loc);
        }
        default:
            return new HIRUnaryOper(node->op, value, value->getType(), node->loc);
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
                error("initializer type mismatch");
            }
        } else if (init) {
            rejectMethodSelectorStorage(init, node);
            type = init->getType();
            rejectBareFunctionStorage(type, node);
        } else {
            error("cannot infer variable type without initializer");
        }

        auto *obj = type->newObj(Object::VARIABLE);
        scope->addObj(node->getName(), obj);
        return new HIRVarDef(toStdString(node->getName()), obj, init, node->loc);
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
        return new HIRRet(expr, node->loc);
    }

    HIRNode *analyzeIf(AstIf *node) {
        auto *cond = requireNonCallExpr(node->condition);
        auto *thenBlock = analyzeBlock(node->then);
        auto *elseBlock = node->hasElse() ? analyzeBlock(node->els) : nullptr;
        return new HIRIf(cond, thenBlock, elseBlock, node->loc);
    }

    HIRNode *analyzeFor(AstFor *node) {
        auto *cond = requireNonCallExpr(node->expr);
        auto *body = analyzeBlock(node->body);
        return new HIRFor(cond, body, node->loc);
    }

    HIRExpr *analyzeSelector(AstSelector *node) {
        auto *parent = requireExpr(node->parent);
        auto *structType = parent->getType() ? parent->getType()->as<StructType>() : nullptr;
        if (!structType) {
            error("selector parent must be a struct value");
        }

        auto fieldName = toStdString(node->field->text);
        auto *member = structType->getMember(llvm::StringRef(fieldName));
        if (member) {
            return new HIRSelector(parent, fieldName, member->first, node->loc);
        }
        if (structType->getFunc(llvm::StringRef(fieldName))) {
            return new HIRSelector(parent, fieldName, nullptr, node->loc);
        }
        error("unknown struct field");
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
        return new HIRCall(callee, std::move(args), retType, node->loc);
    }

public:
    FunctionAnalyzer(TypeTable *typeMgr, GlobalScope *global, AstFuncDecl *node,
                     StructType *methodParent = nullptr)
        : typeMgr(typeMgr),
          global(global),
          scope(new FuncScope(global)),
          methodParent(methodParent),
          hirFunc(nullptr) {
        Function *lofunc = nullptr;
        if (methodParent) {
            lofunc = methodParent->getFunc(
                llvm::StringRef(node->name.tochara(), node->name.size()));
        } else {
            auto *globalFunc = global->getObj(node->name);
            lofunc = globalFunc ? globalFunc->as<Function>() : nullptr;
        }
        if (!lofunc) {
            error("function declaration missing");
        }

        auto *funcType = lofunc->getType()->as<FuncType>();
        if (!funcType) {
            error("invalid function type");
        }

        hirFunc = new HIRFunc(llvm::cast<llvm::Function>(lofunc->getllvmValue()),
                              funcType, node->loc, false,
                              node->body && node->body->hasTerminator());

        if (methodParent) {
            auto *selfObj = methodParent->newObj(Object::VARIABLE);
            scope->addObj(llvm::StringRef("self"), selfObj);
            hirFunc->setSelfBinding({"self", selfObj, node->loc});
        }

        if (node->args) {
            for (auto *argNode : *node->args) {
                auto *decl = argNode->as<AstVarDecl>();
                if (!decl) {
                    error("invalid function argument declaration");
                }
                auto *type = requireType(
                    decl->typeNode,
                    "unknown function argument type for `" + toStdString(decl->field) + "`");
                auto *argObj = type->newObj(Object::VARIABLE);
                scope->addObj(decl->field, argObj);
                hirFunc->addParam({toStdString(decl->field), argObj, decl->loc});
            }
        }

        hirFunc->setBody(analyzeBlock(node->body));
    }

    FunctionAnalyzer(TypeTable *typeMgr, GlobalScope *global, llvm::Function *llvmFunc,
                     AstNode *body, bool skipDeclStatements, const location &loc)
        : typeMgr(typeMgr),
          global(global),
          scope(new FuncScope(global)),
          hirFunc(new HIRFunc(llvmFunc, getOrCreateMainType(typeMgr), loc, true)),
          skipDeclStatements(skipDeclStatements) {
        hirFunc->setBody(analyzeBlock(body));
    }

    HIRFunc *getFunction() const { return hirFunc; }
};

class ModuleAnalyzer {
    GlobalScope *global;
    TypeTable *typeMgr;
    HIRModule *module = new HIRModule();

    void analyzeProgram(AstNode *root) {
        if (auto *program = root->as<AstProgram>()) {
            analyzeTopLevel(program->body);
            return;
        }
        analyzeTopLevel(root);
    }

    void analyzeTopLevel(AstNode *root) {
        auto *body = root->as<AstStatList>();
        if (!body) {
            error("program body must be a statement list");
        }

        bool hasTopLevelExec = false;
        for (auto *stmt : body->getBody()) {
            if (auto *structDecl = stmt->as<AstStructDecl>()) {
                analyzeStruct(structDecl);
                continue;
            }
            if (auto *funcDecl = stmt->as<AstFuncDecl>()) {
                module->addFunction(
                    FunctionAnalyzer(typeMgr, global, funcDecl).getFunction());
                continue;
            }
            hasTopLevelExec = true;
        }

        if (hasTopLevelExec) {
            auto *entry = getOrCreateTopLevelEntry(global, typeMgr);
            module->addFunction(
                FunctionAnalyzer(typeMgr, global, entry, body, true, body->loc)
                    .getFunction());
        }
    }

    void analyzeStruct(AstStructDecl *node) {
        auto *structType = typeMgr->getType(node->name)->as<StructType>();
        if (!structType) {
            error("struct declaration missing");
        }
        if (!node->body || !node->body->is<AstStatList>()) {
            return;
        }
        for (auto *stmt : node->body->as<AstStatList>()->getBody()) {
            auto *func = stmt->as<AstFuncDecl>();
            if (!func) {
                continue;
            }
            module->addFunction(
                FunctionAnalyzer(typeMgr, global, func, structType).getFunction());
        }
    }

public:
    explicit ModuleAnalyzer(GlobalScope *global)
        : global(global), typeMgr(requireTypeTable(global)) {}

    HIRModule *analyze(AstNode *root) {
        analyzeProgram(root);
        return module;
    }
};

}  // namespace

HIRModule *
analyzeModule(GlobalScope *global, AstNode *root) {
    return ModuleAnalyzer(global).analyze(root);
}

}  // namespace lona
