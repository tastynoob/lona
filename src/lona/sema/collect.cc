#include "../visitor.hh"
#include "../type/buildin.hh"
#include "../type/scope.hh"
#include "lona/ast/astnode.hh"
#include "lona/sym/func.hh"
#include "lona/type/type.hh"
#include "parser.hh"
#include <cassert>
#include <cstddef>
#include <list>
#include <llvm-18/llvm/BinaryFormat/Dwarf.h>
#include <llvm-18/llvm/IR/BasicBlock.h>
#include <llvm-18/llvm/IR/Constants.h>
#include <llvm-18/llvm/IR/DIBuilder.h>
#include <llvm-18/llvm/IR/DebugInfoMetadata.h>
#include <llvm-18/llvm/IR/DerivedTypes.h>
#include <llvm-18/llvm/IR/Function.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/Type.h>
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

std::string
sourceFilename(const location &loc) {
    return loc.begin.filename ? *loc.begin.filename : std::string();
}

unsigned
sourceLine(const location &loc) {
    return loc.begin.line > 0 ? static_cast<unsigned>(loc.begin.line) : 1U;
}

unsigned
sourceColumn(const location &loc) {
    return loc.begin.column > 0 ? static_cast<unsigned>(loc.begin.column) : 1U;
}

struct DebugInfoContext {
    llvm::Module &module;
    llvm::DIBuilder builder;
    llvm::DICompileUnit *compileUnit = nullptr;
    llvm::DIFile *primaryFile = nullptr;
    std::unordered_map<std::string, llvm::DIFile *> files;
    std::unordered_map<TypeClass *, llvm::DIType *> types;

    explicit DebugInfoContext(llvm::Module &module, const std::string &sourcePath)
        : module(module), builder(module) {
        module.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                             llvm::DEBUG_METADATA_VERSION);
        module.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
        primaryFile = getOrCreateFile(sourcePath.empty() ? module.getName().str()
                                                         : sourcePath);
        compileUnit = builder.createCompileUnit(
            llvm::dwarf::DW_LANG_C, primaryFile, "lona", false, "", 0);
    }

    llvm::DIFile *getOrCreateFile(const std::string &path) {
        auto key = path.empty() ? std::string("<unknown>") : path;
        auto found = files.find(key);
        if (found != files.end()) {
            return found->second;
        }

        std::string directory = ".";
        std::string filename = key;
        auto slash = key.find_last_of("/\\");
        if (slash != std::string::npos) {
            directory = slash == 0 ? "/" : key.substr(0, slash);
            filename = key.substr(slash + 1);
        }
        if (filename.empty()) {
            filename = module.getName().str();
        }

        auto *file = builder.createFile(filename, directory);
        files.emplace(key, file);
        return file;
    }

    llvm::DIFile *fileForLocation(const location &loc) {
        auto path = sourceFilename(loc);
        if (path.empty()) {
            return primaryFile;
        }
        return getOrCreateFile(path);
    }

    void finalize() {
        builder.finalize();
    }
};

llvm::DIType *
getOrCreateDebugType(DebugInfoContext &debug, TypeClass *type) {
    if (!type) {
        return nullptr;
    }

    auto found = debug.types.find(type);
    if (found != debug.types.end()) {
        return found->second;
    }

    llvm::DIType *diType = nullptr;
    if (auto *base = type->as<BaseType>()) {
        unsigned encoding = llvm::dwarf::DW_ATE_signed;
        switch (base->type) {
        case BaseType::BOOL:
            encoding = llvm::dwarf::DW_ATE_boolean;
            break;
        case BaseType::F32:
        case BaseType::F64:
            encoding = llvm::dwarf::DW_ATE_float;
            break;
        case BaseType::U8:
        case BaseType::U16:
        case BaseType::U32:
        case BaseType::U64:
            encoding = llvm::dwarf::DW_ATE_unsigned;
            break;
        default:
            break;
        }
        diType = debug.builder.createBasicType(
            toStdString(type->full_name), static_cast<uint64_t>(type->typeSize) * 8,
            encoding);
    } else if (auto *pointer = type->as<PointerType>()) {
        diType = debug.builder.createPointerType(
            getOrCreateDebugType(debug, pointer->getPointeeType()),
            static_cast<uint64_t>(type->typeSize) * 8, 0, std::nullopt,
            toStdString(type->full_name));
    } else if (auto *array = type->as<ArrayType>()) {
        diType = debug.builder.createPointerType(
            getOrCreateDebugType(debug, array->getElementType()),
            static_cast<uint64_t>(type->typeSize) * 8, 0, std::nullopt,
            toStdString(type->full_name));
    } else if (auto *func = type->as<FuncType>()) {
        std::vector<llvm::Metadata *> elements;
        elements.reserve(func->getArgTypes().size() + 1);
        elements.push_back(getOrCreateDebugType(debug, func->getRetType()));
        for (auto *argType : func->getArgTypes()) {
            elements.push_back(getOrCreateDebugType(debug, argType));
        }
        diType = debug.builder.createSubroutineType(
            debug.builder.getOrCreateTypeArray(elements));
    } else if (type->as<StructType>()) {
        diType = debug.builder.createStructType(
            debug.primaryFile, toStdString(type->full_name), debug.primaryFile, 1,
            static_cast<uint64_t>(type->typeSize) * 8, 0,
            llvm::DINode::FlagZero, nullptr,
            debug.builder.getOrCreateArray({}));
    } else {
        diType = debug.builder.createUnspecifiedType(toStdString(type->full_name));
    }

    debug.types[type] = diType;
    return diType;
}

llvm::DISubroutineType *
createDebugSubroutineType(DebugInfoContext &debug, FuncType *type) {
    std::vector<llvm::Metadata *> elements;
    if (type) {
        elements.reserve(type->getArgTypes().size() + 1);
        elements.push_back(getOrCreateDebugType(debug, type->getRetType()));
        for (auto *argType : type->getArgTypes()) {
            elements.push_back(getOrCreateDebugType(debug, argType));
        }
    }
    return debug.builder.createSubroutineType(
        debug.builder.getOrCreateTypeArray(elements));
}

llvm::DIScope *
debugScopeFor(DebugInfoContext &debug, llvm::Function *llvmFunc) {
    if (auto *subprogram = llvmFunc->getSubprogram()) {
        return subprogram;
    }
    return debug.primaryFile;
}

llvm::DISubprogram *
createDebugSubprogram(DebugInfoContext &debug, llvm::Function *llvmFunc,
                      FuncType *funcType, const std::string &name,
                      const location &loc) {
    auto *file = debug.fileForLocation(loc);
    auto *subprogram = debug.builder.createFunction(
        file, name, llvmFunc->getName(), file, sourceLine(loc),
        createDebugSubroutineType(debug, funcType), sourceLine(loc),
        llvm::DINode::FlagPrototyped,
        llvm::DISubprogram::SPFlagDefinition);
    llvmFunc->setSubprogram(subprogram);
    return subprogram;
}

void
applyDebugLocation(llvm::IRBuilder<> &builder, DebugInfoContext *debug,
                   llvm::DIScope *scope, const location &loc) {
    if (!debug || !scope) {
        return;
    }
    builder.SetCurrentDebugLocation(llvm::DILocation::get(
        builder.getContext(), sourceLine(loc), sourceColumn(loc), scope));
}

void
clearDebugLocation(llvm::IRBuilder<> &builder) {
    builder.SetCurrentDebugLocation(llvm::DebugLoc());
}

void
emitDebugDeclare(DebugInfoContext *debug, FuncScope *scope, llvm::DIScope *dbgScope,
                 Object *obj, const std::string &name, TypeClass *type,
                 const location &loc, unsigned argNo = 0) {
    if (!debug || !scope || !dbgScope || !obj || !obj->getllvmValue() || !type) {
        return;
    }

    auto *file = debug->fileForLocation(loc);
    auto *var = argNo == 0
        ? debug->builder.createAutoVariable(dbgScope, name, file, sourceLine(loc),
                                            getOrCreateDebugType(*debug, type))
        : debug->builder.createParameterVariable(
              dbgScope, name, argNo, file, sourceLine(loc),
              getOrCreateDebugType(*debug, type), true);

    debug->builder.insertDeclare(
        obj->getllvmValue(), var, debug->builder.createExpression(),
        llvm::DILocation::get(scope->builder.getContext(), sourceLine(loc),
                              sourceColumn(loc), dbgScope),
        scope->builder.GetInsertBlock());
}

std::string
describeTypeNode(TypeNode *node) {
    if (!node) {
        return "void";
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

TypeTable *
requireTypeTable(Scope *scope) {
    assert(scope);
    auto *typeMgr = scope->types();
    assert(typeMgr);
    return typeMgr;
}

StructType *
declareStructType(TypeTable *typeMgr, AstStructDecl *node) {
    auto *existing = typeMgr->getType(node->name);
    if (existing != nullptr) {
        return existing->as<StructType>();
    }

    auto struct_name = node->name;
    auto *structTy = llvm::StructType::create(
        typeMgr->getContext(),
        llvm::StringRef(struct_name.tochara(), struct_name.size()));
    auto *lostructTy = new StructType(structTy, struct_name);

    typeMgr->addType(struct_name, lostructTy);
    return lostructTy;
}

Function *
declareFunction(Scope &scope, TypeTable *typeMgr, AstFuncDecl *node,
                StructType *methodParent) {
    auto &func_name = node->name;
    if (methodParent) {
        if (auto *existing = methodParent->getFunc(
                llvm::StringRef(func_name.tochara(), func_name.size()))) {
            return existing;
        }
    } else if (auto *existing = scope.getObj(func_name)) {
        return existing->as<Function>();
    }

    std::vector<TypeClass *> loargs;
    std::vector<llvm::Type *> args;

    TypeClass *loretType = nullptr;
    llvm::Type *retType = llvm::Type::getVoidTy(typeMgr->getContext());

    std::string funcType_name = "f";
    if (node->retType) {
        loretType = typeMgr->getType(node->retType);
        if (!loretType) {
            throw std::runtime_error(
                "unknown return type for function `" + toStdString(func_name) +
                "`: " + describeTypeNode(node->retType));
        }
        retType = loretType->getLLVMType();
        funcType_name += "_";
        funcType_name.append(loretType->full_name.tochara(),
                             loretType->full_name.size());

        if (loretType->shouldReturnByPointer()) {
            auto *sroa_retType = typeMgr->createPointerType(loretType);
            loargs.push_back(sroa_retType);
            args.push_back(sroa_retType->getLLVMType());
        }
    }

    if (methodParent) {
        loargs.push_back(methodParent);
        args.push_back(methodParent->getLLVMType());
        funcType_name += ".self_";
        funcType_name.append(methodParent->full_name.tochara(),
                             methodParent->full_name.size());
    }

    if (node->args) {
        for (auto *arg : *node->args) {
            if (!arg->is<AstVarDecl>()) {
                throw std::runtime_error(
                    "invalid function parameter declaration in `" +
                    toStdString(func_name) + "`");
            }
            auto *varDecl = arg->as<AstVarDecl>();
            auto *type = typeMgr->getType(varDecl->typeNode);
            if (!type) {
                throw std::runtime_error(
                    "unknown type for function parameter `" +
                    toStdString(varDecl->field) + "` in `" +
                    toStdString(func_name) + "`: " +
                    describeTypeNode(varDecl->typeNode));
            }
            loargs.push_back(type);
            args.push_back(type->getLLVMType());
            funcType_name += ".";
            funcType_name.append(type->full_name.tochara(),
                                 type->full_name.size());
        }
    }

    TypeClass *lofuncType = typeMgr->getType(funcType_name);
    if (!lofuncType) {
        auto *llfuncType = llvm::FunctionType::get(retType, args, false);
        lofuncType = new FuncType(
            llfuncType, std::move(loargs), loretType, func_name, 0);
        typeMgr->addType(funcType_name, lofuncType);
    }

    std::string llvmName = toStdString(func_name);
    if (methodParent) {
        llvmName = toStdString(methodParent->full_name) + "." + llvmName;
    }

    auto *llfunc = llvm::Function::Create(
        (llvm::FunctionType *)lofuncType->llvmType,
        llvm::Function::ExternalLinkage,
        llvm::Twine(llvmName),
        typeMgr->getModule());
    auto *lofunc = new Function(llfunc, lofuncType->as<FuncType>());

    if (methodParent) {
        methodParent->addFunc(
            llvm::StringRef(func_name.tochara(), func_name.size()), lofunc);
    } else {
        scope.addObj(func_name, lofunc);
    }
    return lofunc;
}

}  // namespace

class StructVisitor : public AstVisitorAny {
    TypeTable *typeMgr;

    std::vector<llvm::Type *> llvmmembers;
    llvm::StringMap<StructType::ValueTy> members;

    using AstVisitorAny::visit;

    Object *visit(AstStatList *node) override {
        for (auto it = node->getBody().begin(); it != node->getBody().end();
             it++) {
            (*it)->accept(*this);
        }
        return nullptr;
    }

    Object *visit(AstVarDecl *node) override {
        auto &name = node->field;
        auto *type = typeMgr->getType(node->typeNode);
        if (!type) {
            throw std::runtime_error("unknown struct field type for `" +
                                     toStdString(name) + "`: " +
                                     describeTypeNode(node->typeNode));
        }

        llvmmembers.push_back(type->getLLVMType());
        members.insert({llvm::StringRef(name.tochara(), name.size()),
                        {type, static_cast<int>(llvmmembers.size() - 1)}});

        return nullptr;
    }

public:
    StructVisitor(TypeTable *typeMgr, AstStructDecl *node) : typeMgr(typeMgr) {
        auto *lostructTy = declareStructType(typeMgr, node);
        assert(lostructTy);
        assert(lostructTy->llvmType);

        if (!lostructTy->isOpaque()) {
            return;
        }

        this->visit(node->body);

        ((llvm::StructType *)lostructTy->llvmType)->setBody(llvmmembers);

        auto typesize =
            typeMgr->getModule().getDataLayout().getTypeSizeInBits(
                lostructTy->llvmType) /
            8;

        lostructTy->complete(members, typesize);
    }
};

class TypeCollector : public AstVisitorAny {
    TypeTable *typeMgr;
    Scope *scope;

    std::list<AstStructDecl *> structDecls;
    std::list<AstFuncDecl *> funcDecls;

    using AstVisitorAny::visit;

    Object *visit(AstProgram *node) override {
        this->visit(node->body);
        return nullptr;
    }

    Object *visit(AstStatList *node) override {
        for (auto *it : node->body) {
            if (it->is<AstStructDecl>()) {
                structDecls.push_back(it->as<AstStructDecl>());
            } else if (it->is<AstFuncDecl>()) {
                funcDecls.push_back(it->as<AstFuncDecl>());
            }
        }
        return nullptr;
    }

    Object *visit(AstStructDecl *node) override {
        auto *structTy = declareStructType(typeMgr, node);
        if (!node->body || !node->body->is<AstStatList>()) {
            return nullptr;
        }
        for (auto *stmt : node->body->as<AstStatList>()->getBody()) {
            auto *func = stmt->as<AstFuncDecl>();
            if (!func) {
                continue;
            }
            declareFunction(*scope, typeMgr, func, structTy);
        }
        return nullptr;
    }

    Object *visit(AstFuncDecl *node) override {
        declareFunction(*scope, typeMgr, node, nullptr);
        return nullptr;
    }

public:
    TypeCollector(TypeTable *typeMgr, Scope *scope, AstNode *root)
        : typeMgr(typeMgr), scope(scope) {
        this->visit(root);

        for (auto *it : structDecls) {
            this->visit(it);
        }

        for (auto *it : funcDecls) {
            this->visit(it);
        }

        for (auto *it : structDecls) {
            StructVisitor(typeMgr, it);
        }
    }
};

Function *
createFunc(Scope &scope, AstFuncDecl *root, StructType *parent) {
    initBuildinType(&scope);
    auto *func = declareFunction(scope, requireTypeTable(&scope), root, parent);
    return func;
}

void
scanningType(Scope *global, AstNode *root) {
    initBuildinType(global);
    TypeCollector(requireTypeTable(global), global, root);
}

StructType *
createStruct(Scope *scope, AstStructDecl *node) {
    initBuildinType(scope);
    auto *typeMgr = requireTypeTable(scope);
    auto *type = declareStructType(typeMgr, node);
    if (type->isOpaque()) {
        StructVisitor(typeMgr, node);
    }
    return type;
}

namespace {

class FunctionCompiler : public AstVisitor {
    TypeTable *typeMgr;
    GlobalScope *global;
    FuncScope *scope;
    llvm::LLVMContext &context;
    StructType *methodParent;
    DebugInfoContext *debug;
    llvm::DISubprogram *debugSubprogram = nullptr;
    bool skipDeclStatements = false;

    using AstVisitor::visit;

    [[noreturn]] void error(const std::string &message) {
        throw std::runtime_error(message);
    }

    Object *materializeLocal(TypeClass *type, Object *initVal) {
        auto *obj = type->newObj(Object::VARIABLE);
        obj->createllvmValue(scope);
        if (initVal != nullptr) {
            obj->set(scope->builder, initVal);
        }
        return obj;
    }

    void setLocation(AstNode *node) {
        if (!node) {
            return;
        }
        applyDebugLocation(scope->builder, debug, debugSubprogram, node->loc);
    }

    void clearLocation() {
        clearDebugLocation(scope->builder);
    }

    Object *compileToValue(AstNode *node) {
        auto *obj = this->visit(node);
        if (!obj) {
            error("expression did not produce a value");
        }
        return obj;
    }

    llvm::Value *emitBoolCast(Object *obj) {
        auto *value = obj->get(scope->builder);
        auto *type = obj->getType();
        if (type == boolTy) {
            return value;
        }
        if (type == i32Ty) {
            return scope->builder.CreateICmpNE(
                value, llvm::ConstantInt::get(i32Ty->getLLVMType(), 0));
        }
        error("unsupported condition type");
    }

    Object *emitBinaryIntOp(token_type op, Object *left, Object *right) {
        if (left->getType() != right->getType()) {
            error("type mismatch in binary operation");
        }
        if (left->getType() != i32Ty) {
            error("only i32 binary operations are supported");
        }

        auto *lhs = left->get(scope->builder);
        auto *rhs = right->get(scope->builder);
        llvm::Value *result = nullptr;
        TypeClass *resultType = i32Ty;

        switch (op) {
        case '+':
            result = scope->builder.CreateAdd(lhs, rhs);
            break;
        case '-':
            result = scope->builder.CreateSub(lhs, rhs);
            break;
        case '*':
            result = scope->builder.CreateMul(lhs, rhs);
            break;
        case '/':
            result = scope->builder.CreateSDiv(lhs, rhs);
            break;
        case '<':
            result = scope->builder.CreateICmpSLT(lhs, rhs);
            resultType = boolTy;
            break;
        case '>':
            result = scope->builder.CreateICmpSGT(lhs, rhs);
            resultType = boolTy;
            break;
        case Parser::token::LOGIC_EQUAL:
            result = scope->builder.CreateICmpEQ(lhs, rhs);
            resultType = boolTy;
            break;
        case Parser::token::LOGIC_NOT_EQUAL:
            result = scope->builder.CreateICmpNE(lhs, rhs);
            resultType = boolTy;
            break;
        default:
            error("unsupported binary operator");
        }

        auto *obj = resultType->newObj(Object::REG_VAL | Object::READONLY);
        obj->bindllvmValue(result);
        return obj;
    }

    void ensureTerminatorForCurrentBlock() {
        auto *block = scope->builder.GetInsertBlock();
        if (!block || block->getTerminator()) {
            return;
        }
        if (scope->retBlock() && block != scope->retBlock()) {
            scope->builder.CreateBr(scope->retBlock());
            return;
        }
        if (scope->retVal()) {
            scope->builder.CreateRet(scope->retVal()->get(scope->builder));
        } else {
            scope->builder.CreateRetVoid();
        }
    }

    Object *visit(AstProgram *node) override {
        return this->visit(node->body);
    }

    Object *visit(AstStatList *node) override {
        Object *last = nullptr;
        for (auto *stmt : node->getBody()) {
            if (skipDeclStatements &&
                (stmt->is<AstStructDecl>() || stmt->is<AstFuncDecl>())) {
                continue;
            }
            if (scope->builder.GetInsertBlock()->getTerminator()) {
                break;
            }
            last = this->visit(stmt);
        }
        return last;
    }

    Object *visit(AstConst *node) override {
        switch (node->getType()) {
        case AstConst::Type::INT32:
            return new ConstVar(i32Ty, *node->getBuf<int32_t>());
        default:
            error("only i32 constants are supported");
        }
    }

    Object *visit(AstField *node) override {
        auto *obj = scope->getObj(node->name);
        if (!obj) {
            error("undefined identifier");
        }
        return obj;
    }

    Object *visit(AstAssign *node) override {
        setLocation(node);
        auto *dst = compileToValue(node->left);
        auto *src = compileToValue(node->right);
        dst->set(scope->builder, src);
        return dst;
    }

    Object *visit(AstBinOper *node) override {
        setLocation(node);
        auto *left = compileToValue(node->left);
        auto *right = compileToValue(node->right);
        return emitBinaryIntOp(node->op, left, right);
    }

    Object *visit(AstUnaryOper *node) override {
        setLocation(node);
        auto *value = compileToValue(node->expr);
        auto *llvmValue = value->get(scope->builder);
        switch (node->op) {
        case '+':
            if (value->getType() != i32Ty) {
                error("unary + expects i32");
            }
            return value;
        case '-': {
            if (value->getType() != i32Ty) {
                error("unary - expects i32");
            }
            auto *result = i32Ty->newObj(Object::REG_VAL | Object::READONLY);
            result->bindllvmValue(scope->builder.CreateNeg(llvmValue));
            return result;
        }
        case '!': {
            auto *result = boolTy->newObj(Object::REG_VAL | Object::READONLY);
            result->bindllvmValue(scope->builder.CreateNot(emitBoolCast(value)));
            return result;
        }
        default:
            error("unsupported unary operator");
        }
    }

    Object *visit(AstVarDef *node) override {
        setLocation(node);
        Object *initVal = nullptr;
        if (node->withInitVal()) {
            initVal = compileToValue(node->getInitVal());
        }

        TypeClass *type = nullptr;
        if (auto *typeNode = node->getTypeNode()) {
            type = typeMgr->getType(typeNode);
            if (!type) {
                error("unknown variable type: " + describeTypeNode(typeNode));
            }
            if (initVal && initVal->getType() != type) {
                error("initializer type mismatch");
            }
        } else if (initVal) {
            type = initVal->getType();
        } else {
            error("cannot infer variable type without initializer");
        }

        auto *obj = materializeLocal(type, initVal);
        scope->addObj(node->getName(), obj);
        emitDebugDeclare(debug, scope, debugSubprogram, obj,
                         toStdString(node->getName()), type, node->loc);
        return obj;
    }

    Object *visit(AstRet *node) override {
        setLocation(node);
        auto *retSlot = scope->retVal();
        if (node->expr) {
            auto *value = compileToValue(node->expr);
            if (!retSlot) {
                error("unexpected return value in void function");
            }
            retSlot->set(scope->builder, value);
        } else if (retSlot) {
            error("missing return value");
        }

        if (scope->retBlock()) {
            scope->builder.CreateBr(scope->retBlock());
        } else if (retSlot) {
            scope->builder.CreateRet(retSlot->get(scope->builder));
        } else {
            scope->builder.CreateRetVoid();
        }
        scope->setReturned();
        return retSlot;
    }

    Object *visit(AstIf *node) override {
        setLocation(node);
        auto *condObj = compileToValue(node->condition);
        auto *llvmFunc = scope->builder.GetInsertBlock()->getParent();

        auto *thenBB = llvm::BasicBlock::Create(context, "if.then", llvmFunc);
        auto *mergeBB = llvm::BasicBlock::Create(context, "if.end");
        auto *elseBB = node->hasElse()
            ? llvm::BasicBlock::Create(context, "if.else")
            : mergeBB;

        scope->builder.CreateCondBr(emitBoolCast(condObj), thenBB, elseBB);

        scope->builder.SetInsertPoint(thenBB);
        this->visit(node->then);
        if (!scope->builder.GetInsertBlock()->getTerminator()) {
            scope->builder.CreateBr(mergeBB);
        }

        if (node->hasElse()) {
            llvmFunc->insert(llvmFunc->end(), elseBB);
            scope->builder.SetInsertPoint(elseBB);
            this->visit(node->els);
            if (!scope->builder.GetInsertBlock()->getTerminator()) {
                scope->builder.CreateBr(mergeBB);
            }
        }

        llvmFunc->insert(llvmFunc->end(), mergeBB);
        scope->builder.SetInsertPoint(mergeBB);
        return nullptr;
    }

    Object *visit(AstSelector *node) override {
        setLocation(node);
        auto *parent = compileToValue(node->parent);
        auto fieldName = toStdString(node->field->text);

        if (auto *structParent = parent->as<StructVar>()) {
            return structParent->getField(scope->builder, fieldName);
        }

        error("selector parent must be a struct value");
    }

    Object *visit(AstFieldCall *node) override {
        setLocation(node);
        std::vector<Object *> args;
        Function *callee = nullptr;

        if (node->value->is<AstSelector>()) {
            auto *selector = node->value->as<AstSelector>();
            auto *parent = compileToValue(selector->parent);
            auto *structType = parent->getType()->as<StructType>();
            if (!structType) {
                error("selector call parent must be a struct value");
            }
            callee = structType->getFunc(
                llvm::StringRef(selector->field->text.tochara(),
                                selector->field->text.size()));
            if (!callee) {
                error("unknown struct method");
            }
            args.push_back(parent);
        } else {
            callee = compileToValue(node->value)->as<Function>();
            if (!callee) {
                error("only direct function calls are supported");
            }
        }

        if (node->args) {
            args.reserve(args.size() + node->args->size());
            for (auto *arg : *node->args) {
                args.push_back(compileToValue(arg));
            }
        }
        return callee->call(scope, args);
    }

public:
    FunctionCompiler(TypeTable *typeMgr, GlobalScope *global, AstFuncDecl *node,
                     StructType *methodParent = nullptr,
                     DebugInfoContext *debug = nullptr)
        : typeMgr(typeMgr),
          global(global),
          scope(nullptr),
          context(global->module.getContext()),
          methodParent(methodParent),
          debug(debug) {
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

        auto *llvmFunc = llvm::cast<llvm::Function>(lofunc->getllvmValue());
        if (!llvmFunc->empty()) {
            return;
        }

        auto *entry = llvm::BasicBlock::Create(context, "entry", llvmFunc);
        global->builder.SetInsertPoint(entry);
        scope = new FuncScope(global);
        scope->structTy = methodParent;

        auto *funcType = lofunc->getType()->as<FuncType>();
        if (!funcType) {
            error("invalid function type");
        }
        if (funcType->SROA()) {
            error("by-pointer returns are not supported yet");
        }

        if (debug) {
            debugSubprogram = createDebugSubprogram(
                *debug, llvmFunc, funcType, llvmFunc->getName().str(), node->loc);
            scope->builder.SetCurrentDebugLocation(llvm::DILocation::get(
                context, sourceLine(node->loc), sourceColumn(node->loc),
                debugSubprogram));
        }

        if (auto *retType = funcType->getRetType()) {
            if (!node->body || !node->body->hasTerminator()) {
                error("missing return value");
            }
            auto *retSlot = materializeLocal(retType, nullptr);
            scope->initRetVal(retSlot);
            auto *retBB = llvm::BasicBlock::Create(context, "return", llvmFunc);
            scope->initRetBlock(retBB);
        }

        size_t llvmArgIndex = 0;
        unsigned debugArgIndex = 1;
        if (methodParent) {
            auto *selfObj = methodParent->newObj(Object::VARIABLE);
            selfObj->createllvmValue(scope);
            auto argIt = llvmFunc->arg_begin();
            std::advance(argIt, llvmArgIndex);
            scope->builder.CreateStore(&*argIt, selfObj->getllvmValue());
            scope->addObj(llvm::StringRef("self"), selfObj);
            emitDebugDeclare(debug, scope, debugSubprogram, selfObj, "self",
                             methodParent, node->loc, debugArgIndex);
            ++llvmArgIndex;
            ++debugArgIndex;
        }

        if (node->args) {
            auto expectedArgs = node->args->size() + (methodParent ? 1 : 0);
            if (llvmFunc->arg_size() != expectedArgs) {
                error("function argument number mismatch");
            }
            for (auto *argNode : *node->args) {
                auto *decl = argNode->as<AstVarDecl>();
                if (!decl) {
                    error("invalid function argument declaration");
                }
                auto *type = typeMgr->getType(decl->typeNode);
                if (!type) {
                    error("unknown function argument type for `" +
                          toStdString(decl->field) + "`: " +
                          describeTypeNode(decl->typeNode));
                }
                auto *argObj = materializeLocal(type, nullptr);
                auto argIt = llvmFunc->arg_begin();
                std::advance(argIt, llvmArgIndex);
                scope->builder.CreateStore(&*argIt, argObj->getllvmValue());
                scope->addObj(decl->field, argObj);
                emitDebugDeclare(debug, scope, debugSubprogram, argObj,
                                 toStdString(decl->field), type, decl->loc,
                                 debugArgIndex);
                ++llvmArgIndex;
                ++debugArgIndex;
            }
        } else if (llvmFunc->arg_size() != llvmArgIndex) {
            error("function argument number mismatch");
        }

        this->visit(node->body);

        if (scope->retBlock()) {
            if (!scope->builder.GetInsertBlock()->getTerminator()) {
                scope->builder.CreateBr(scope->retBlock());
            }
            scope->builder.SetInsertPoint(scope->retBlock());
            if (debugSubprogram) {
                scope->builder.SetCurrentDebugLocation(llvm::DILocation::get(
                    context, sourceLine(node->loc), sourceColumn(node->loc),
                    debugSubprogram));
            }
        }

        ensureTerminatorForCurrentBlock();
        clearLocation();
    }

    FunctionCompiler(TypeTable *typeMgr, GlobalScope *global,
                     llvm::Function *llvmFunc, TypeClass *retType, AstNode *body,
                     bool skipDeclStatements, const location &loc,
                     DebugInfoContext *debug = nullptr)
        : typeMgr(typeMgr),
          global(global),
          scope(nullptr),
          context(global->module.getContext()),
          methodParent(nullptr),
          debug(debug),
          skipDeclStatements(skipDeclStatements) {
        if (!llvmFunc->empty()) {
            return;
        }

        auto *entry = llvm::BasicBlock::Create(context, "entry", llvmFunc);
        global->builder.SetInsertPoint(entry);
        scope = new FuncScope(global);

        if (debug) {
            auto *mainType = typeMgr->getType(llvm::StringRef("f_i32"))->as<FuncType>();
            debugSubprogram = createDebugSubprogram(
                *debug, llvmFunc, mainType, llvmFunc->getName().str(), loc);
            scope->builder.SetCurrentDebugLocation(llvm::DILocation::get(
                context, sourceLine(loc), sourceColumn(loc), debugSubprogram));
        }

        if (retType) {
            auto *retSlot = materializeLocal(retType, nullptr);
            scope->initRetVal(retSlot);
            if (retType == i32Ty) {
                retSlot->set(scope->builder, new ConstVar(i32Ty, int32_t(0)));
            }
            auto *retBB = llvm::BasicBlock::Create(context, "return", llvmFunc);
            scope->initRetBlock(retBB);
        }

        if (llvmFunc->arg_size() != 0) {
            error("top-level entry function cannot accept arguments");
        }

        this->visit(body);

        if (scope->retBlock()) {
            if (!scope->builder.GetInsertBlock()->getTerminator()) {
                scope->builder.CreateBr(scope->retBlock());
            }
            scope->builder.SetInsertPoint(scope->retBlock());
            if (debugSubprogram) {
                scope->builder.SetCurrentDebugLocation(llvm::DILocation::get(
                    context, sourceLine(loc), sourceColumn(loc), debugSubprogram));
            }
        }

        ensureTerminatorForCurrentBlock();
        clearLocation();
    }
};


class ModuleCompiler : public AstVisitorAny {
    GlobalScope *global;
    TypeTable *typeMgr;
    DebugInfoContext *debug;

    using AstVisitorAny::visit;

    Object *visit(AstProgram *node) override {
        return this->visit(node->body);
    }

    Object *visit(AstStatList *node) override {
        bool hasTopLevelExec = false;
        for (auto *stmt : node->getBody()) {
            if (stmt->is<AstStructDecl>()) {
                this->visit(stmt);
                continue;
            }
            if (stmt->is<AstFuncDecl>()) {
                this->visit(stmt);
                continue;
            }
            hasTopLevelExec = true;
        }

        if (hasTopLevelExec) {
            auto *mainType = typeMgr->getType(llvm::StringRef("f_i32"));
            if (!mainType) {
                auto *llvmMainType =
                    llvm::FunctionType::get(i32Ty->getLLVMType(), false);
                mainType = new FuncType(llvmMainType, {}, i32Ty, "f_i32", 0);
                typeMgr->addType(llvm::StringRef("f_i32"), mainType);
            }

            auto moduleName = global->module.getName();
            std::string entryName = moduleName.str() + ".main";
            auto *llvmMain = global->module.getFunction(entryName);
            if (!llvmMain) {
                llvmMain = llvm::Function::Create(
                    llvm::cast<llvm::FunctionType>(mainType->getLLVMType()),
                    llvm::Function::ExternalLinkage,
                    llvm::Twine(entryName), global->module);
            }
            FunctionCompiler(typeMgr, global, llvmMain, i32Ty, node, true,
                             node->loc, debug);
        }
        return nullptr;
    }

    Object *visit(AstStructDecl *node) override {
        auto *structTy = typeMgr->getType(node->name)->as<StructType>();
        if (!structTy) {
            throw std::runtime_error("struct declaration missing");
        }
        if (!node->body || !node->body->is<AstStatList>()) {
            return nullptr;
        }
        for (auto *stmt : node->body->as<AstStatList>()->getBody()) {
            auto *func = stmt->as<AstFuncDecl>();
            if (!func) {
                continue;
            }
            FunctionCompiler(typeMgr, global, func, structTy, debug);
        }
        return nullptr;
    }

    Object *visit(AstFuncDecl *node) override {
        FunctionCompiler(typeMgr, global, node, nullptr, debug);
        return nullptr;
    }

public:
    ModuleCompiler(GlobalScope *global, AstNode *root,
                   DebugInfoContext *debug = nullptr)
        : global(global), typeMgr(requireTypeTable(global)), debug(debug) {
        this->visit(root);
    }
};


}  // namespace

void
compileModule(Scope *global, AstNode *root, bool emitDebugInfo) {
    auto *globalScope = dynamic_cast<GlobalScope *>(global);
    assert(globalScope);
    initBuildinType(globalScope);

    std::unique_ptr<DebugInfoContext> debug;
    if (emitDebugInfo) {
        debug = std::make_unique<DebugInfoContext>(
            globalScope->module, globalScope->module.getName().str());
    }

    ModuleCompiler(globalScope, root, debug.get());

    if (debug) {
        debug->finalize();
    }
}

}  // namespace lona
