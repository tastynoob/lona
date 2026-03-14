#include "../visitor.hh"
#include "../type/buildin.hh"
#include "../type/scope.hh"
#include "lona/ast/astnode.hh"
#include "lona/sym/func.hh"
#include "lona/sema/hir.hh"
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

void
rejectBareFunctionType(TypeClass *type, TypeNode *node, const std::string &context) {
    if (!type || !type->as<FuncType>()) {
        return;
    }
    throw std::runtime_error(context + ": " + describeTypeNode(node));
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
    bool returnByPointer = false;

    std::string funcType_name = "f";
    if (node->retType) {
        loretType = typeMgr->getType(node->retType);
        if (!loretType) {
            throw std::runtime_error(
                "unknown return type for function `" + toStdString(func_name) +
                "`: " + describeTypeNode(node->retType));
        }
        rejectBareFunctionType(
            loretType, node->retType,
            "unsupported bare function return type for `" + toStdString(func_name) + "`");
        returnByPointer = loretType->shouldReturnByPointer();
        retType = returnByPointer
            ? llvm::Type::getVoidTy(typeMgr->getContext())
            : loretType->getLLVMType();
        funcType_name += "_";
        funcType_name.append(loretType->full_name.tochara(),
                             loretType->full_name.size());

        if (returnByPointer) {
            auto *sroa_retType = typeMgr->createPointerType(loretType);
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
            rejectBareFunctionType(
                type, varDecl->typeNode,
                "unsupported bare function parameter type for `" +
                    toStdString(varDecl->field) + "` in `" +
                    toStdString(func_name) + "`");
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
        rejectBareFunctionType(
            type, node->typeNode,
            "unsupported bare function struct field type for `" +
                toStdString(name) + "`");

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
            declareStructType(typeMgr, it);
        }

        for (auto *it : structDecls) {
            StructVisitor(typeMgr, it);
        }

        for (auto *it : structDecls) {
            this->visit(it);
        }

        for (auto *it : funcDecls) {
            this->visit(it);
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

class FunctionCompiler {
    TypeTable *typeMgr;
    GlobalScope *global;
    FuncScope *scope;
    llvm::LLVMContext &context;
    DebugInfoContext *debug;
    llvm::DISubprogram *debugSubprogram = nullptr;
    bool returnByPointer = false;

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

    Object *materializeBinding(Object *obj, Object *initVal = nullptr) {
        if (!obj) {
            error("missing binding object");
        }
        obj->createllvmValue(scope);
        if (initVal != nullptr) {
            obj->set(scope->builder, initVal);
        }
        return obj;
    }

    void setLocation(const location &loc) {
        applyDebugLocation(scope->builder, debug, debugSubprogram, loc);
    }

    void setLocation(HIRNode *node) {
        if (node) {
            setLocation(node->getLocation());
        }
    }

    void clearLocation() {
        clearDebugLocation(scope->builder);
    }

    Object *compileExpr(HIRExpr *expr) {
        if (!expr) {
            return nullptr;
        }
        if (auto *value = dynamic_cast<HIRValue *>(expr)) {
            return value->getValue();
        }
        if (auto *assign = dynamic_cast<HIRAssign *>(expr)) {
            setLocation(assign);
            auto *dst = compileExpr(assign->getLeft());
            auto *src = compileExpr(assign->getRight());
            if (!dst || !src) {
                error("assignment requires values");
            }
            dst->set(scope->builder, src);
            return dst;
        }
        if (auto *bin = dynamic_cast<HIRBinOper *>(expr)) {
            setLocation(bin);
            auto *left = compileExpr(bin->getLeft());
            auto *right = compileExpr(bin->getRight());
            return emitBinaryIntOp(bin->getOp(), left, right);
        }
        if (auto *unary = dynamic_cast<HIRUnaryOper *>(expr)) {
            setLocation(unary);
            auto *value = compileExpr(unary->getExpr());
            switch (unary->getOp()) {
            case '+': {
                if (value->getType() != i32Ty) {
                    error("unary + expects i32");
                }
                return value;
            }
            case '-': {
                auto *llvmValue = value->get(scope->builder);
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
            case '&': {
                if (!value->isVariable() || value->isRegVal() || !value->getllvmValue()) {
                    error("address-of expects an addressable value");
                }
                auto *pointerType = unary->getType() ? unary->getType()->as<PointerType>()
                                                     : nullptr;
                if (!pointerType) {
                    error("address-of expects a pointer result type");
                }
                auto *result = pointerType->newObj(Object::REG_VAL | Object::READONLY);
                result->bindllvmValue(value->getllvmValue());
                return result;
            }
            case '*': {
                auto *pointerType = value->getType() ? value->getType()->as<PointerType>()
                                                     : nullptr;
                if (!pointerType) {
                    error("dereference expects a pointer value");
                }
                auto *result = pointerType->getPointeeType()->newObj(Object::VARIABLE);
                result->setllvmValue(value->get(scope->builder));
                return result;
            }
            default:
                error("unsupported unary operator");
            }
        }
        if (auto *selector = dynamic_cast<HIRSelector *>(expr)) {
            setLocation(selector);
            auto *parent = compileExpr(selector->getParent());
            auto fieldName = selector->getFieldName();
            if (auto *structParent = parent->as<StructVar>()) {
                if (selector->getType() == nullptr) {
                    error(kMethodSelectorDirectCallError);
                }
                return structParent->getField(scope->builder, fieldName);
            }
            error("selector parent must be a struct value");
        }
        if (auto *call = dynamic_cast<HIRCall *>(expr)) {
            setLocation(call);
            std::vector<Object *> args;
            Function *callee = nullptr;

            if (auto *selector = dynamic_cast<HIRSelector *>(call->getCallee())) {
                auto *parent = compileExpr(selector->getParent());
                auto *structType = parent->getType()->as<StructType>();
                if (!structType) {
                    error("selector call parent must be a struct value");
                }
                callee = structType->getFunc(llvm::StringRef(selector->getFieldName()));
                if (!callee) {
                    error("unknown struct method");
                }
                args.push_back(parent);
            } else if (auto *calleeValue = dynamic_cast<HIRValue *>(call->getCallee())) {
                callee = calleeValue->getValue()->as<Function>();
                if (!callee) {
                    error("only direct function calls are supported");
                }
            } else {
                error("only direct function calls are supported");
            }

            args.reserve(args.size() + call->getArgs().size());
            for (auto *arg : call->getArgs()) {
                auto *value = compileExpr(arg);
                if (!value) {
                    error("call argument did not produce a value");
                }
                args.push_back(value);
            }
            return callee->call(scope, args);
        }
        error("unsupported HIR expression");
    }

    Object *compileNode(HIRNode *node) {
        if (!node) {
            return nullptr;
        }
        if (auto *block = dynamic_cast<HIRBlock *>(node)) {
            return compileBlock(block);
        }
        if (auto *varDef = dynamic_cast<HIRVarDef *>(node)) {
            setLocation(varDef);
            Object *initVal = nullptr;
            if (varDef->getInit()) {
                initVal = compileExpr(varDef->getInit());
            }
            auto *obj = materializeBinding(varDef->getObject(), initVal);
            scope->addObj(varDef->getName(), obj);
            emitDebugDeclare(debug, scope, debugSubprogram, obj, varDef->getName(),
                             obj->getType(), varDef->getLocation());
            return obj;
        }
        if (auto *ret = dynamic_cast<HIRRet *>(node)) {
            setLocation(ret);
            auto *retSlot = scope->retVal();
            if (ret->getExpr()) {
                auto *value = compileExpr(ret->getExpr());
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
        if (auto *ifNode = dynamic_cast<HIRIf *>(node)) {
            setLocation(ifNode);
            auto *condObj = compileExpr(ifNode->getCondition());
            auto *llvmFunc = scope->builder.GetInsertBlock()->getParent();

            auto *thenBB = llvm::BasicBlock::Create(context, "if.then", llvmFunc);
            auto *mergeBB = llvm::BasicBlock::Create(context, "if.end");
            auto *elseBB = ifNode->hasElseBlock()
                ? llvm::BasicBlock::Create(context, "if.else")
                : mergeBB;

            scope->builder.CreateCondBr(emitBoolCast(condObj), thenBB, elseBB);

            scope->builder.SetInsertPoint(thenBB);
            compileBlock(ifNode->getThenBlock());
            if (!scope->builder.GetInsertBlock()->getTerminator()) {
                scope->builder.CreateBr(mergeBB);
            }

            if (ifNode->hasElseBlock()) {
                llvmFunc->insert(llvmFunc->end(), elseBB);
                scope->builder.SetInsertPoint(elseBB);
                compileBlock(ifNode->getElseBlock());
                if (!scope->builder.GetInsertBlock()->getTerminator()) {
                    scope->builder.CreateBr(mergeBB);
                }
            }

            llvmFunc->insert(llvmFunc->end(), mergeBB);
            scope->builder.SetInsertPoint(mergeBB);
            return nullptr;
        }
        if (auto *forNode = dynamic_cast<HIRFor *>(node)) {
            setLocation(forNode);
            auto *llvmFunc = scope->builder.GetInsertBlock()->getParent();
            auto *condBB = llvm::BasicBlock::Create(context, "for.cond", llvmFunc);
            auto *bodyBB = llvm::BasicBlock::Create(context, "for.body");
            auto *endBB = llvm::BasicBlock::Create(context, "for.end");

            scope->builder.CreateBr(condBB);

            scope->builder.SetInsertPoint(condBB);
            auto *condObj = compileExpr(forNode->getCondition());
            scope->builder.CreateCondBr(emitBoolCast(condObj), bodyBB, endBB);

            llvmFunc->insert(llvmFunc->end(), bodyBB);
            scope->builder.SetInsertPoint(bodyBB);
            compileBlock(forNode->getBody());
            if (!scope->builder.GetInsertBlock()->getTerminator()) {
                scope->builder.CreateBr(condBB);
            }

            llvmFunc->insert(llvmFunc->end(), endBB);
            scope->builder.SetInsertPoint(endBB);
            return nullptr;
        }
        if (auto *expr = dynamic_cast<HIRExpr *>(node)) {
            return compileExpr(expr);
        }
        error("unsupported HIR node");
    }

    Object *compileBlock(HIRBlock *block) {
        Object *last = nullptr;
        if (!block) {
            return last;
        }
        for (auto *stmt : block->getBody()) {
            auto *insertBlock = scope->builder.GetInsertBlock();
            if (!insertBlock || insertBlock->getTerminator()) {
                break;
            }
            last = compileNode(stmt);
        }
        return last;
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
            if (returnByPointer) {
                scope->builder.CreateRetVoid();
            } else {
                scope->builder.CreateRet(scope->retVal()->get(scope->builder));
            }
        } else {
            scope->builder.CreateRetVoid();
        }
    }

public:
    FunctionCompiler(TypeTable *typeMgr, GlobalScope *global, HIRFunc *hirFunc,
                     DebugInfoContext *debug = nullptr)
        : typeMgr(typeMgr),
          global(global),
          scope(nullptr),
          context(global->module.getContext()),
          debug(debug) {
        if (!hirFunc) {
            error("missing HIR function");
        }

        auto *llvmFunc = hirFunc->getLLVMFunction();
        if (!llvmFunc) {
            error("HIR function missing LLVM function");
        }
        if (!llvmFunc->empty()) {
            return;
        }

        auto *entry = llvm::BasicBlock::Create(context, "entry", llvmFunc);
        global->builder.SetInsertPoint(entry);
        scope = new FuncScope(global);

        auto *funcType = hirFunc->getFuncType();
        if (!funcType) {
            error("invalid function type");
        }
        returnByPointer = funcType->SROA();

        if (hirFunc->hasSelfBinding()) {
            scope->structTy = hirFunc->getSelfBinding().object->getType()->as<StructType>();
        }

        if (debug) {
            debugSubprogram = createDebugSubprogram(
                *debug, llvmFunc, funcType, llvmFunc->getName().str(),
                hirFunc->getLocation());
            scope->builder.SetCurrentDebugLocation(llvm::DILocation::get(
                context, sourceLine(hirFunc->getLocation()),
                sourceColumn(hirFunc->getLocation()), debugSubprogram));
        }

        auto *retType = funcType->getRetType();
        if (!hirFunc->isTopLevelEntry() && retType && !hirFunc->hasGuaranteedReturn()) {
            error("missing return value");
        }

        size_t llvmArgIndex = 0;
        if (retType) {
            Object *retSlot = nullptr;
            if (returnByPointer) {
                if (llvmFunc->arg_size() < 1) {
                    error("function is missing hidden return slot argument");
                }
                auto argIt = llvmFunc->arg_begin();
                retSlot = retType->newObj(Object::VARIABLE);
                retSlot->setllvmValue(&*argIt);
                ++llvmArgIndex;
            } else {
                retSlot = materializeLocal(retType, nullptr);
            }
            scope->initRetVal(retSlot);
            if (hirFunc->isTopLevelEntry() && retType == i32Ty) {
                retSlot->set(scope->builder, new ConstVar(i32Ty, int32_t(0)));
            }
            auto *retBB = llvm::BasicBlock::Create(context, "return", llvmFunc);
            scope->initRetBlock(retBB);
        }

        unsigned debugArgIndex = 1;
        if (hirFunc->hasSelfBinding()) {
            auto &binding = hirFunc->getSelfBinding();
            auto *selfObj = materializeBinding(binding.object);
            auto argIt = llvmFunc->arg_begin();
            std::advance(argIt, llvmArgIndex);
            scope->builder.CreateStore(&*argIt, selfObj->getllvmValue());
            scope->addObj(llvm::StringRef(binding.name), selfObj);
            emitDebugDeclare(debug, scope, debugSubprogram, selfObj, binding.name,
                             selfObj->getType(), binding.loc, debugArgIndex);
            ++llvmArgIndex;
            ++debugArgIndex;
        }

        auto expectedArgs = hirFunc->getParams().size() +
            (hirFunc->hasSelfBinding() ? 1 : 0) + (returnByPointer ? 1 : 0);
        if (llvmFunc->arg_size() != expectedArgs) {
            error("function argument number mismatch");
        }
        for (const auto &binding : hirFunc->getParams()) {
            auto *argObj = materializeBinding(binding.object);
            auto argIt = llvmFunc->arg_begin();
            std::advance(argIt, llvmArgIndex);
            scope->builder.CreateStore(&*argIt, argObj->getllvmValue());
            scope->addObj(llvm::StringRef(binding.name), argObj);
            emitDebugDeclare(debug, scope, debugSubprogram, argObj, binding.name,
                             argObj->getType(), binding.loc, debugArgIndex);
            ++llvmArgIndex;
            ++debugArgIndex;
        }

        compileBlock(hirFunc->getBody());

        if (scope->retBlock()) {
            auto *insertBlock = scope->builder.GetInsertBlock();
            if (insertBlock && !insertBlock->getTerminator()) {
                scope->builder.CreateBr(scope->retBlock());
            }
            scope->builder.SetInsertPoint(scope->retBlock());
            if (debugSubprogram) {
                scope->builder.SetCurrentDebugLocation(llvm::DILocation::get(
                    context, sourceLine(hirFunc->getLocation()),
                    sourceColumn(hirFunc->getLocation()), debugSubprogram));
            }
        }

        ensureTerminatorForCurrentBlock();
        clearLocation();
    }
};

class ModuleCompiler {
    GlobalScope *global;
    TypeTable *typeMgr;
    DebugInfoContext *debug;

public:
    ModuleCompiler(GlobalScope *global, HIRModule *module,
                   DebugInfoContext *debug = nullptr)
        : global(global), typeMgr(requireTypeTable(global)), debug(debug) {
        for (auto *func : module->getFunctions()) {
            FunctionCompiler(typeMgr, global, func, debug);
        }
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

    auto *hirModule = analyzeModule(globalScope, root);
    ModuleCompiler(globalScope, hirModule, debug.get());

    if (debug) {
        debug->finalize();
    }
}

}  // namespace lona
