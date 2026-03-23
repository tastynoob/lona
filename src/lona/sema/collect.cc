#include "../visitor.hh"
#include "../abi/abi.hh"
#include "../abi/native_abi.hh"
#include "../type/buildin.hh"
#include "../type/scope.hh"
#include "lona/ast/astnode.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/array_dim.hh"
#include "lona/err/err.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/operator_resolver.hh"
#include "lona/sym/func.hh"
#include "lona/sema/hir.hh"
#include "parser.hh"
#include <cassert>
#include <cstddef>
#include <cstdint>
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

[[noreturn]] void
error(const std::string &message);

[[noreturn]] void
error(const location &loc, const std::string &message,
      const std::string &hint);

[[noreturn]] void
internalError(const std::string &message,
              const std::string &hint = std::string());

enum class TopLevelDeclKind {
    StructType,
    Function,
};

const char *
topLevelDeclKindName(TopLevelDeclKind kind) {
    switch (kind) {
    case TopLevelDeclKind::StructType:
        return "struct";
    case TopLevelDeclKind::Function:
        return "top-level function";
    }
    return "top-level declaration";
}

void
recordTopLevelDeclName(
    std::unordered_map<std::string, std::pair<TopLevelDeclKind, location>> &seen,
    const std::string &name, TopLevelDeclKind kind, const location &loc) {
    auto found = seen.find(name);
    if (found != seen.end()) {
        if (found->second.first != kind) {
            error(loc,
                  std::string(topLevelDeclKindName(kind)) + " `" + name +
                      "` conflicts with " +
                      topLevelDeclKindName(found->second.first) + " `" + name +
                      "`",
                  "Type names reserve constructor syntax like `" + name +
                      "(...)`. Rename the function, for example `make" + name +
                      "`, or choose a different type name.");
        }
        return;
    }
    seen.emplace(name, std::make_pair(kind, loc));
}

std::vector<std::string>
extractParamNames(AstFuncDecl *node) {
    std::vector<std::string> names;
    if (!node || !node->args) {
        return names;
    }
    names.reserve(node->args->size());
    for (auto *arg : *node->args) {
        auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
        if (!varDecl) {
            continue;
        }
        names.push_back(toStdString(varDecl->field));
    }
    return names;
}

std::vector<BindingKind>
extractParamBindingKinds(AstFuncDecl *node, bool withImplicitSelf = false) {
    std::vector<BindingKind> kinds;
    if (withImplicitSelf) {
        kinds.push_back(BindingKind::Ref);
    }
    if (!node || !node->args) {
        return kinds;
    }
    kinds.reserve(kinds.size() + node->args->size());
    for (auto *arg : *node->args) {
        auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
        kinds.push_back(varDecl ? varDecl->bindingKind : BindingKind::Value);
    }
    return kinds;
}

std::string
describeExternCFunctionName(AstFuncDecl *node, StructType *methodParent) {
    if (!node) {
        return "<unknown>";
    }
    auto name = toStdString(node->name);
    if (!methodParent) {
        return name;
    }
    return toStdString(methodParent->full_name) + "." + name;
}

std::string
describeExternCTypeSubject(const std::string &role, const std::string &name) {
    if (name.empty()) {
        return role;
    }
    return role + " `" + name + "`";
}

std::string
describeExternCType(TypeClass *type, TypeNode *node) {
    if (node) {
        return describeTypeNode(node, type ? toStdString(type->full_name) : "void");
    }
    if (type) {
        return toStdString(type->full_name);
    }
    return "void";
}

bool
isExternCCallbackType(TypeClass *type) {
    if (!type) {
        return false;
    }
    if (auto *qualified = type->as<ConstType>()) {
        return isExternCCallbackType(qualified->getBaseType());
    }
    if (auto *pointer = asUnqualified<PointerType>(type)) {
        auto *pointeeType = pointer->getPointeeType();
        return (pointeeType && pointeeType->as<FuncType>()) ||
               isExternCCallbackType(pointeeType);
    }
    if (auto *indexable = asUnqualified<IndexablePointerType>(type)) {
        auto *elementType = indexable->getElementType();
        return (elementType && elementType->as<FuncType>()) ||
               isExternCCallbackType(elementType);
    }
    if (auto *array = asUnqualified<ArrayType>(type)) {
        return isExternCCallbackType(array->getElementType());
    }
    return false;
}

bool
isExternCByValueAggregateType(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    return storageType &&
        (storageType->as<StructType>() || storageType->as<TupleType>() ||
         storageType->as<ArrayType>());
}

bool
isCCompatibleStructIdentity(StructType *type) {
    return type && (type->isExternDecl() || type->isReprC());
}

bool
isCCompatiblePointerTarget(TypeClass *type) {
    if (!type) {
        return false;
    }
    if (auto *qualified = type->as<ConstType>()) {
        return isCCompatiblePointerTarget(qualified->getBaseType());
    }
    if (type->as<BaseType>()) {
        return true;
    }
    if (auto *structType = type->as<StructType>()) {
        return isCCompatibleStructIdentity(structType);
    }
    if (auto *pointerType = type->as<PointerType>()) {
        auto *pointeeType = pointerType->getPointeeType();
        return pointeeType && !pointeeType->as<FuncType>() &&
            isCCompatiblePointerTarget(pointeeType);
    }
    if (auto *indexableType = type->as<IndexablePointerType>()) {
        auto *elementType = indexableType->getElementType();
        return elementType && !elementType->as<FuncType>() &&
            isCCompatiblePointerTarget(elementType);
    }
    return false;
}

bool
isCCompatibleReprCFieldType(TypeClass *type) {
    if (!type) {
        return false;
    }
    if (auto *qualified = type->as<ConstType>()) {
        return isCCompatibleReprCFieldType(qualified->getBaseType());
    }
    if (type->as<BaseType>()) {
        return true;
    }
    if (auto *pointerType = type->as<PointerType>()) {
        return isCCompatiblePointerTarget(pointerType->getPointeeType());
    }
    if (auto *indexableType = type->as<IndexablePointerType>()) {
        return isCCompatiblePointerTarget(indexableType->getElementType());
    }
    if (auto *structType = type->as<StructType>()) {
        return structType->isReprC();
    }
    if (auto *arrayType = type->as<ArrayType>()) {
        return arrayType->hasStaticLayout() &&
            isCCompatibleReprCFieldType(arrayType->getElementType());
    }
    return false;
}

void
rejectOpaqueStructByValue(TypeClass *type, TypeNode *typeNode,
                          const location &loc, const std::string &context) {
    auto *structType = asUnqualified<StructType>(type);
    if (!structType || !structType->isExternDecl()) {
        return;
    }
    auto typeName = describeExternCType(type, typeNode);
    error(loc,
          "opaque extern struct `" + typeName +
              "` cannot be used by value in " + context,
          "Use `" + typeName +
              "*` instead. Opaque C structs are only supported behind pointers.");
}

void
validateStructDeclShape(AstStructDecl *node) {
    if (!node) {
        return;
    }
    if (node->isExternDecl() && node->body) {
        error(node->loc,
              "extern struct `" + toStdString(node->name) +
                  "` cannot declare fields or methods",
              "Use `extern struct " + toStdString(node->name) +
                  "` for an opaque C type, or drop `extern` and declare a normal struct body.");
    }
}

void
validateExternCType(AstFuncDecl *node, StructType *methodParent,
                    const std::string &role, const std::string &bindingName,
                    TypeClass *type, TypeNode *typeNode, const location &loc) {
    if (!node || !node->isExternC() || !type) {
        return;
    }

    auto funcName = describeExternCFunctionName(node, methodParent);
    auto subject = describeExternCTypeSubject(role, bindingName);
    auto typeName = describeExternCType(type, typeNode);

    if (isExternCCallbackType(type)) {
        error(loc,
              "extern \"C\" function `" + funcName +
                  "` uses unsupported " + subject + ": " + typeName,
              "Callback support is not implemented in C FFI v0 yet.");
    }
    if (auto *pointerType = asUnqualified<PointerType>(type)) {
        if (!isCCompatiblePointerTarget(pointerType->getPointeeType())) {
            error(loc,
                  "extern \"C\" function `" + funcName +
                      "` uses unsupported " + subject + ": " + typeName,
                  "Use pointers to scalars, pointers, `extern struct`, or `repr(\"C\") struct` types. Ordinary Lona structs cannot cross the C FFI boundary.");
        }
        return;
    }
    if (auto *indexableType = asUnqualified<IndexablePointerType>(type)) {
        if (!isCCompatiblePointerTarget(indexableType->getElementType())) {
            error(loc,
                  "extern \"C\" function `" + funcName +
                      "` uses unsupported " + subject + ": " + typeName,
                  "Use pointers to scalars, pointers, `extern struct`, or `repr(\"C\") struct` types. Ordinary Lona structs cannot cross the C FFI boundary.");
        }
        return;
    }
    if (asUnqualified<TupleType>(type)) {
        error(loc,
              "extern \"C\" function `" + funcName +
                  "` uses unsupported " + subject + ": " + typeName,
              "Flatten the tuple into scalar parameters or pass a pointer instead.");
    }
    if (isExternCByValueAggregateType(type)) {
        error(loc,
              "extern \"C\" function `" + funcName +
                  "` uses unsupported " + subject + ": " + typeName,
              "Pass a pointer instead. C FFI v0 does not support aggregate values at the boundary yet.");
    }
}

void
validateStructFieldType(AstStructDecl *structDecl, AstVarDecl *fieldDecl,
                        TypeClass *fieldType) {
    if (!structDecl || !fieldDecl || !fieldType) {
        return;
    }
    if (!isFullyWritableStructFieldType(fieldType)) {
        error(fieldDecl->loc,
              "struct field `" + toStdString(fieldDecl->field) +
                  "` cannot use a const-qualified storage type",
              "Struct fields must keep full read/write access during initialization. Use pointer views like `T const*` or `T const[*]`, and reserve field immutability for a future `readonly` feature.");
    }
    rejectOpaqueStructByValue(fieldType, fieldDecl->typeNode, fieldDecl->loc,
                              "struct field `" + toStdString(fieldDecl->field) + "`");
    if (!structDecl->isReprC()) {
        return;
    }
    if (isCCompatibleReprCFieldType(fieldType)) {
        return;
    }

    auto fieldTypeName = describeExternCType(fieldType, fieldDecl->typeNode);
    error(fieldDecl->loc,
          "repr(\"C\") struct `" + toStdString(structDecl->name) +
              "` field `" + toStdString(fieldDecl->field) +
              "` uses unsupported type: " + fieldTypeName,
          "Use only C-compatible field types: scalars, raw pointers, fixed arrays of C-compatible elements, or nested `repr(\"C\")` structs.");
}

void
validateExternCFunctionSignature(AstFuncDecl *node, StructType *methodParent,
                                 const std::vector<TypeClass *> &argTypes,
                                 TypeClass *retType) {
    if (!node || !node->isExternC()) {
        return;
    }

    auto funcName = describeExternCFunctionName(node, methodParent);
    if (methodParent) {
        error(node->loc,
              "extern \"C\" method `" + funcName + "` is not supported",
              "Declare a top-level wrapper function instead. C FFI v0 only supports top-level functions.");
    }

    size_t argTypeIndex = 0;
    if (node->args) {
        for (auto *arg : *node->args) {
            auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
            if (!varDecl) {
                continue;
            }
            if (varDecl->bindingKind == BindingKind::Ref) {
                error(varDecl->loc,
                      "extern \"C\" function `" + funcName +
                          "` parameter `" + toStdString(varDecl->field) +
                          "` cannot use `ref` binding",
                      "Use an explicit pointer type like `i32*` instead.");
            }
            auto *argType = argTypeIndex < argTypes.size() ? argTypes[argTypeIndex]
                                                           : nullptr;
            rejectOpaqueStructByValue(
                argType, varDecl->typeNode, varDecl->loc,
                "parameter `" + toStdString(varDecl->field) + "` in function `" +
                    funcName + "`");
            validateExternCType(node, methodParent, "parameter",
                                toStdString(varDecl->field), argType,
                                varDecl->typeNode, varDecl->loc);
            ++argTypeIndex;
        }
    }

    rejectOpaqueStructByValue(retType, node->retType, node->loc,
                              "return type of function `" + funcName + "`");
    validateExternCType(node, methodParent, "return type", std::string(),
                        retType, node->retType, node->loc);
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
internalError(const std::string &message, const std::string &hint) {
    throw DiagnosticError(DiagnosticError::Category::Internal, message, hint);
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
    TypeTable &typeTable;
    llvm::DIBuilder builder;
    llvm::DICompileUnit *compileUnit = nullptr;
    llvm::DIFile *primaryFile = nullptr;
    std::unordered_map<std::string, llvm::DIFile *> files;
    std::unordered_map<TypeClass *, llvm::DIType *> types;

    explicit DebugInfoContext(llvm::Module &module, TypeTable &types,
                              const std::string &sourcePath)
        : module(module), typeTable(types), builder(module) {
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
    if (auto *base = asUnqualified<BaseType>(type)) {
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
            toStdString(type->full_name),
            debug.typeTable.getTypeAllocSize(type) * 8,
            encoding);
    } else if (auto *pointer = asUnqualified<PointerType>(type)) {
        diType = debug.builder.createPointerType(
            getOrCreateDebugType(debug, pointer->getPointeeType()),
            debug.typeTable.getTypeAllocSize(type) * 8, 0, std::nullopt,
            toStdString(type->full_name));
    } else if (auto *indexable = asUnqualified<IndexablePointerType>(type)) {
        diType = debug.builder.createPointerType(
            getOrCreateDebugType(debug, indexable->getElementType()),
            debug.typeTable.getTypeAllocSize(type) * 8, 0, std::nullopt,
            toStdString(type->full_name));
    } else if (auto *array = asUnqualified<ArrayType>(type)) {
        diType = debug.builder.createPointerType(
            getOrCreateDebugType(debug, array->getElementType()),
            debug.typeTable.getTypeAllocSize(type) * 8, 0, std::nullopt,
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
    } else if (asUnqualified<StructType>(type)) {
        diType = debug.builder.createStructType(
            debug.primaryFile, toStdString(type->full_name), debug.primaryFile, 1,
            debug.typeTable.getTypeAllocSize(type) * 8, 0,
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
resolveTopLevelName(const CompilationUnit *unit, const string &name,
                    bool exportNamespace) {
    auto resolved = toStdString(name);
    if (!unit || !exportNamespace) {
        return resolved;
    }
    return unit->moduleName() + "." + resolved;
}

std::string
resolveFunctionSymbolName(const CompilationUnit *unit, const string &name,
                          AbiKind abiKind, bool exportNamespace) {
    if (abiKind == AbiKind::C) {
        return toStdString(name);
    }
    return resolveTopLevelName(unit, name, exportNamespace);
}

bool isReservedInitialListTypeName(llvm::StringRef name);
[[noreturn]] void errorReservedInitialListType(const location &loc);

TypeClass *
resolveTypeNode(TypeTable *typeMgr, const CompilationUnit *unit, TypeNode *node) {
    if (!typeMgr) {
        return nullptr;
    }
    if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
        auto rawName = llvm::StringRef(base->name.tochara(), base->name.size());
        if (isReservedInitialListTypeName(rawName)) {
            errorReservedInitialListType(base->loc);
        }
    }
    return unit ? unit->resolveType(typeMgr, node) : typeMgr->getType(node);
}

void
rejectBareFunctionType(TypeClass *type, TypeNode *node, const std::string &context,
                       const location &loc = location()) {
    if (!type || !type->as<FuncType>()) {
        return;
    }
    error(loc, context + ": " + describeTypeNode(node, "void"),
          "Use an explicit function pointer type instead of a bare function type.");
}

TypeTable *
requireTypeTable(Scope *scope) {
    assert(scope);
    auto *typeMgr = scope->types();
    assert(typeMgr);
    return typeMgr;
}

void
declareModuleNamespace(Scope &scope, const CompilationUnit &unit) {
    auto moduleName = llvm::StringRef(unit.moduleName());
    auto *existing = scope.getObj(moduleName);
    if (existing) {
        auto *moduleObject = existing->as<ModuleObject>();
        if (!moduleObject || moduleObject->unit() != &unit) {
            error("module namespace `" + unit.moduleName() +
                  "` conflicts with an existing global symbol");
        }
        return;
    }
    scope.addObj(moduleName, new ModuleObject(&unit));
}

StructType *
declareStructType(TypeTable *typeMgr, AstStructDecl *node,
                  CompilationUnit *unit = nullptr,
                  bool exportNamespace = false) {
    validateStructDeclShape(node);
    auto resolvedName = resolveTopLevelName(unit, node->name, exportNamespace);
    if (unit) {
        if (unit->importsModule(toStdString(node->name))) {
            error(node->loc,
                  "struct `" + toStdString(node->name) +
                      "` conflicts with imported module alias `" +
                      toStdString(node->name) + "`",
                  "Rename the struct so `" + toStdString(node->name) +
                      ".xxx` continues to refer to the imported module.");
        }
        if (unit->findLocalFunction(toStdString(node->name)) != nullptr) {
            error(node->loc,
                  "struct `" + toStdString(node->name) +
                      "` conflicts with top-level function `" +
                      toStdString(node->name) + "`",
                  "Type names reserve constructor syntax like `" +
                      toStdString(node->name) +
                      "(...)`. Rename the function, for example `make" +
                      toStdString(node->name) + "`.");
        }
        unit->bindLocalType(toStdString(node->name), resolvedName);
    }

    auto *existing = typeMgr->getType(llvm::StringRef(resolvedName));
    if (existing != nullptr) {
        auto *existingStruct = existing->as<StructType>();
        if (existingStruct) {
            existingStruct->setDeclKind(node ? node->declKind
                                             : StructDeclKind::Native);
        }
        return existingStruct;
    }

    string struct_name(resolvedName.c_str());
    auto *lostructTy = new StructType(
        struct_name, node ? node->declKind : StructDeclKind::Native);

    typeMgr->addType(struct_name, lostructTy);
    return lostructTy;
}

Function *
declareFunction(Scope &scope, TypeTable *typeMgr, AstFuncDecl *node,
                StructType *methodParent, CompilationUnit *unit = nullptr,
                bool exportNamespace = false) {
    auto &func_name = node->name;
    auto resolvedFunctionName = resolveFunctionSymbolName(
        unit, func_name, node ? node->abiKind : AbiKind::Native, exportNamespace);
    if (methodParent) {
        if (auto *existing = typeMgr->getMethodFunction(
                methodParent,
                llvm::StringRef(func_name.tochara(), func_name.size()))) {
            return existing;
        }
    } else {
        if (unit) {
            if (unit->importsModule(toStdString(func_name))) {
                error(node->loc,
                      "top-level function `" + toStdString(func_name) +
                          "` conflicts with imported module alias `" +
                          toStdString(func_name) + "`",
                      "Rename the function so `" + toStdString(func_name) +
                          ".xxx` continues to refer to the imported module.");
            }
            if (unit->findLocalType(toStdString(func_name)) != nullptr) {
                error(node->loc,
                      "top-level function `" + toStdString(func_name) +
                          "` conflicts with struct `" +
                          toStdString(func_name) + "`",
                      "Type names reserve constructor syntax like `" +
                          toStdString(func_name) +
                          "(...)`. Rename the function, for example `make" +
                          toStdString(func_name) + "`.");
            }
            unit->bindLocalFunction(toStdString(func_name), resolvedFunctionName);
        }
        if (auto *existing = scope.getObj(llvm::StringRef(resolvedFunctionName))) {
            auto *func = existing->as<Function>();
            if (!func) {
                error(node->loc,
                      "top-level function `" + toStdString(func_name) +
                          "` conflicts with module namespace `" +
                          resolvedFunctionName + "`");
            }
            return func;
        }
    }

    std::vector<TypeClass *> loargs;
    auto loargKinds = extractParamBindingKinds(node, methodParent != nullptr);
    TypeClass *loretType = nullptr;
    if (node->retType) {
        loretType = resolveTypeNode(typeMgr, unit, node->retType);
        if (!loretType) {
            error(node->loc,
                "unknown return type for function `" + toStdString(func_name) +
                "`: " + describeTypeNode(node->retType, "void"));
        }
        rejectBareFunctionType(
            loretType, node->retType,
            "unsupported bare function return type for `" + toStdString(func_name) + "`",
            node->loc);
        rejectOpaqueStructByValue(
            loretType, node->retType, node->loc,
            "return type of function `" + toStdString(func_name) + "`");
    }

    if (methodParent) {
        loargs.push_back(methodParent);
    }

    if (node->args) {
        for (auto *arg : *node->args) {
            if (!arg->is<AstVarDecl>()) {
                error(node->loc,
                    "invalid function parameter declaration in `" +
                    toStdString(func_name) + "`");
            }
            auto *varDecl = arg->as<AstVarDecl>();
            auto *type = resolveTypeNode(typeMgr, unit, varDecl->typeNode);
            if (!type) {
                error(varDecl->loc,
                    "unknown type for function parameter `" +
                    toStdString(varDecl->field) + "` in `" +
                    toStdString(func_name) + "`: " +
                    describeTypeNode(varDecl->typeNode, "void"));
            }
            rejectBareFunctionType(
                type, varDecl->typeNode,
                "unsupported bare function parameter type for `" +
                    toStdString(varDecl->field) + "` in `" +
                    toStdString(func_name) + "`",
                varDecl->loc);
            rejectOpaqueStructByValue(
                type, varDecl->typeNode, varDecl->loc,
                "parameter `" + toStdString(varDecl->field) + "` in function `" +
                    toStdString(func_name) + "`");
            loargs.push_back(type);
        }
    }

    validateExternCFunctionSignature(node, methodParent, loargs, loretType);
    auto *lofuncType = typeMgr->getOrCreateFunctionType(loargs, loretType,
                                                        std::move(loargKinds),
                                                        node->abiKind);
    if (methodParent && !methodParent->getMethodType(
            llvm::StringRef(func_name.tochara(), func_name.size()))) {
        methodParent->addMethodType(
            llvm::StringRef(func_name.tochara(), func_name.size()), lofuncType,
            extractParamNames(node));
    }

    std::string llvmName = resolvedFunctionName.empty()
        ? toStdString(func_name)
        : resolvedFunctionName;
    if (methodParent) {
        llvmName = toStdString(methodParent->full_name) + "." + llvmName;
    }

    auto *llfunc = llvm::Function::Create(
        getFunctionAbiLLVMType(*typeMgr, lofuncType, methodParent != nullptr),
        llvm::Function::ExternalLinkage,
        llvm::Twine(llvmName),
        typeMgr->getModule());
    auto *lofunc = new Function(llfunc, lofuncType, extractParamNames(node),
                                methodParent != nullptr);

    if (methodParent) {
        typeMgr->bindMethodFunction(
            methodParent,
            llvm::StringRef(func_name.tochara(), func_name.size()), lofunc);
    } else {
        scope.addObj(llvm::StringRef(llvmName), lofunc);
    }
    return lofunc;
}

}  // namespace

class StructVisitor : public AstVisitorAny {
    TypeTable *typeMgr;
    CompilationUnit *unit;
    bool exportNamespace;
    AstStructDecl *structDecl = nullptr;

    llvm::StringMap<StructType::ValueTy> members;
    int nextMemberIndex = 0;

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
        if (node->bindingKind == BindingKind::Ref) {
            error(node->loc, "struct fields cannot use `ref` binding for `" +
                                 toStdString(name) + "`",
                  "Store an explicit pointer type instead. Struct fields must be value or pointer-like storage.");
        }
        auto *type = resolveTypeNode(typeMgr, unit, node->typeNode);
        if (!type) {
            error(node->loc, "unknown struct field type for `" +
                                 toStdString(name) + "`: " +
                                 describeTypeNode(node->typeNode, "void"));
        }
        rejectBareFunctionType(
            type, node->typeNode,
            "unsupported bare function struct field type for `" +
                toStdString(name) + "`",
            node->loc);
        validateStructFieldType(structDecl, node, type);

        members.insert({llvm::StringRef(name.tochara(), name.size()),
                        {type, nextMemberIndex++}});

        return nullptr;
    }

public:
    StructVisitor(TypeTable *typeMgr, AstStructDecl *node,
                  CompilationUnit *unit = nullptr,
                  bool exportNamespace = false)
        : typeMgr(typeMgr), unit(unit), exportNamespace(exportNamespace),
          structDecl(node) {
        auto *lostructTy = declareStructType(typeMgr, node, unit, exportNamespace);
        assert(lostructTy);

        if (!lostructTy->isOpaque()) {
            return;
        }

        if (!node->body) {
            return;
        }

        this->visit(node->body);
        lostructTy->complete(members);
    }
};

class TypeCollector : public AstVisitorAny {
    TypeTable *typeMgr;
    Scope *scope;
    CompilationUnit *unit;
    bool exportNamespace;

    std::list<AstStructDecl *> structDecls;
    std::list<AstFuncDecl *> funcDecls;
    std::unordered_map<std::string, std::pair<TopLevelDeclKind, location>>
        topLevelDecls;

    using AstVisitorAny::visit;

    Object *visit(AstProgram *node) override {
        this->visit(node->body);
        return nullptr;
    }

    Object *visit(AstStatList *node) override {
        for (auto *it : node->body) {
            if (it->is<AstStructDecl>()) {
                auto *decl = it->as<AstStructDecl>();
                validateStructDeclShape(decl);
                recordTopLevelDeclName(topLevelDecls, toStdString(decl->name),
                                       TopLevelDeclKind::StructType, decl->loc);
                structDecls.push_back(it->as<AstStructDecl>());
            } else if (it->is<AstFuncDecl>()) {
                auto *decl = it->as<AstFuncDecl>();
                recordTopLevelDeclName(topLevelDecls, toStdString(decl->name),
                                       TopLevelDeclKind::Function, decl->loc);
                funcDecls.push_back(it->as<AstFuncDecl>());
            }
        }
        return nullptr;
    }

    Object *visit(AstStructDecl *node) override {
        auto *structTy = declareStructType(typeMgr, node, unit, exportNamespace);
        if (!node->body || !node->body->is<AstStatList>()) {
            return nullptr;
        }
        for (auto *stmt : node->body->as<AstStatList>()->getBody()) {
            auto *func = stmt->as<AstFuncDecl>();
            if (!func) {
                continue;
            }
            declareFunction(*scope, typeMgr, func, structTy, unit, exportNamespace);
        }
        return nullptr;
    }

    Object *visit(AstFuncDecl *node) override {
        declareFunction(*scope, typeMgr, node, nullptr, unit, exportNamespace);
        return nullptr;
    }

public:
    TypeCollector(TypeTable *typeMgr, Scope *scope, AstNode *root,
                  CompilationUnit *unit = nullptr,
                  bool exportNamespace = false)
        : typeMgr(typeMgr),
          scope(scope),
          unit(unit),
          exportNamespace(exportNamespace) {
        this->visit(root);

        for (auto *it : structDecls) {
            declareStructType(typeMgr, it, unit, exportNamespace);
        }

        for (auto *it : structDecls) {
            StructVisitor(typeMgr, it, unit, exportNamespace);
        }

        for (auto *it : structDecls) {
            this->visit(it);
        }

        for (auto *it : funcDecls) {
            this->visit(it);
        }
    }
};

namespace {

TypeClass *
lookupBuiltinType(llvm::StringRef name) {
    if (name == "u8") return u8Ty;
    if (name == "i8") return i8Ty;
    if (name == "u16") return u16Ty;
    if (name == "i16") return i16Ty;
    if (name == "u32") return u32Ty;
    if (name == "i32") return i32Ty;
    if (name == "u64") return u64Ty;
    if (name == "i64") return i64Ty;
    if (name == "int") return i32Ty;
    if (name == "uint") return u32Ty;
    if (name == "f32") return f32Ty;
    if (name == "f64") return f64Ty;
    if (name == "bool") return boolTy;
    return nullptr;
}

bool
isReservedInitialListTypeName(llvm::StringRef name) {
    return name == "initial_list";
}

[[noreturn]] void
errorReservedInitialListType(const location &loc) {
    error(loc,
          "`initial_list` is a compiler-internal initialization interface",
          "Use brace initialization like `{1, 2, 3}` instead. User-visible generic `initial_list<T>` support is not implemented.");
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
          "explicit unsized array type syntax is not allowed: " +
              describeTypeNode(node, "<unknown type>"),
          "Use fixed explicit dimensions like `i32[2]`. If you want inferred array dimensions, write `var a = {1, 2}`. If you need an indexable pointer, write `T[*]`.");
}

[[noreturn]] void
errorLegacyIndexablePointerSyntax(const location &loc, TypeNode *node) {
    error(loc,
          "explicit unsized array type syntax is not allowed inside pointer declarations: " +
              describeTypeNode(node, "<unknown type>"),
          "Use `T[*]` instead, for example `u8[*]`. `[]` is not a user-writable type declaration syntax.");
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
    if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
        validateTypeNodeLayout(qualified->base);
        return;
    }
    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        if (auto *array = dynamic_cast<ArrayTypeNode *>(pointer->base);
            array && hasUnsizedArrayDimensions(array->dim) &&
            isBareUnsizedArraySyntax(array->dim)) {
            errorLegacyIndexablePointerSyntax(pointer->loc, pointer);
        }
        validateTypeNodeLayout(pointer->base);
        return;
    }
    if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
        validateTypeNodeLayout(indexable->base);
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

class InterfaceCollector {
    CompilationUnit &unit_;
    ModuleInterface *interface_;
    std::vector<AstStructDecl *> structDecls_;
    std::vector<AstFuncDecl *> funcDecls_;
    std::unordered_map<std::string, std::pair<TopLevelDeclKind, location>>
        topLevelDecls_;

    void validateImportAliasConflict(AstStructDecl *structDecl) {
        const auto name = toStdString(structDecl->name);
        if (!unit_.importsModule(name)) {
            return;
        }
        error(structDecl->loc,
              "struct `" + name + "` conflicts with imported module alias `" +
                  name + "`",
              "Rename the struct so `" + name +
                  ".xxx` continues to refer to the imported module.");
    }

    void validateImportAliasConflict(AstFuncDecl *funcDecl) {
        const auto name = toStdString(funcDecl->name);
        if (!unit_.importsModule(name)) {
            return;
        }
        error(funcDecl->loc,
              "top-level function `" + name +
                  "` conflicts with imported module alias `" + name + "`",
              "Rename the function so `" + name +
                  ".xxx` continues to refer to the imported module.");
    }

    TypeClass *resolveType(TypeNode *node, bool validateLayout = true) {
        if (!node) {
            return nullptr;
        }
        if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
            return resolveType(param->type, false);
        }
        if (validateLayout) {
            validateTypeNodeLayout(node);
        }
        if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
            auto *baseType = resolveType(qualified->base, false);
            return baseType ? interface_->getOrCreateConstType(baseType) : nullptr;
        }
        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            auto rawName = toStdString(base->name);
            auto separator = rawName.find('.');
            if (separator == std::string::npos) {
                if (isReservedInitialListTypeName(
                        llvm::StringRef(rawName.c_str(), rawName.size()))) {
                    errorReservedInitialListType(base->loc);
                }
                if (auto *builtin = lookupBuiltinType(
                        llvm::StringRef(rawName.c_str(), rawName.size()))) {
                    return builtin;
                }
                auto lookup = interface_->lookupTopLevelName(rawName);
                if (lookup.isType() && lookup.typeDecl) {
                    return lookup.typeDecl->type;
                }
                return nullptr;
            }

            auto moduleName = rawName.substr(0, separator);
            auto typeName = rawName.substr(separator + 1);
            const auto *imported = unit_.findImportedModule(moduleName);
            if (!imported || !imported->interface) {
                return nullptr;
            }
            auto lookup = imported->interface->lookupTopLevelName(typeName);
            return lookup.isType() && lookup.typeDecl ? lookup.typeDecl->type : nullptr;
        }
        if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
            auto *baseType = resolveType(pointer->base, false);
            for (uint32_t i = 0; baseType && i < pointer->dim; ++i) {
                baseType = interface_->getOrCreatePointerType(baseType);
            }
            return baseType;
        }
        if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
            auto *elementType = resolveType(indexable->base, false);
            return elementType ? interface_->getOrCreateIndexablePointerType(elementType)
                               : nullptr;
        }
        if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
            auto *elementType = resolveType(array->base, false);
            if (!elementType) {
                return nullptr;
            }
            return interface_->getOrCreateArrayType(elementType, array->dim);
        }
        if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
            std::vector<TypeClass *> itemTypes;
            itemTypes.reserve(tuple->items.size());
            for (auto *item : tuple->items) {
                auto *itemType = resolveType(item, false);
                if (!itemType) {
                    return nullptr;
                }
                itemTypes.push_back(itemType);
            }
            return interface_->getOrCreateTupleType(itemTypes);
        }
        if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
            std::vector<TypeClass *> argTypes;
            std::vector<BindingKind> argBindingKinds;
            argTypes.reserve(func->args.size());
            argBindingKinds.reserve(func->args.size());
            for (auto *arg : func->args) {
                argBindingKinds.push_back(funcParamBindingKind(arg));
                auto *argType = resolveType(unwrapFuncParamType(arg), false);
                if (!argType) {
                    return nullptr;
                }
                argTypes.push_back(argType);
            }
            auto *retType = resolveType(func->ret, false);
            auto *funcType = interface_->getOrCreateFunctionType(
                argTypes, retType, std::move(argBindingKinds));
            return funcType ? interface_->getOrCreatePointerType(funcType)
                            : nullptr;
        }
        return nullptr;
    }

    void collectTopLevelLists(AstNode *root) {
        auto *program = dynamic_cast<AstProgram *>(root);
        auto *body = dynamic_cast<AstStatList *>(program ? program->body : root);
        if (!body) {
            return;
        }
        for (auto *stmt : body->getBody()) {
            if (auto *structDecl = dynamic_cast<AstStructDecl *>(stmt)) {
                validateImportAliasConflict(structDecl);
                validateStructDeclShape(structDecl);
                recordTopLevelDeclName(topLevelDecls_, toStdString(structDecl->name),
                                       TopLevelDeclKind::StructType,
                                       structDecl->loc);
                structDecls_.push_back(structDecl);
            } else if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt)) {
                validateImportAliasConflict(funcDecl);
                recordTopLevelDeclName(topLevelDecls_, toStdString(funcDecl->name),
                                       TopLevelDeclKind::Function,
                                       funcDecl->loc);
                funcDecls_.push_back(funcDecl);
            }
        }
    }

    void declareStructs() {
        for (auto *structDecl : structDecls_) {
            interface_->declareStructType(toStdString(structDecl->name),
                                          structDecl->declKind);
        }
    }

    void completeStructs() {
        for (auto *structDecl : structDecls_) {
            auto *typeDecl = interface_->findType(toStdString(structDecl->name));
            auto *structType = typeDecl ? typeDecl->type->as<StructType>() : nullptr;
            if (!structType) {
                error(structDecl->loc, "failed to declare struct interface");
            }
            if (structType->isOpaque() == false) {
                continue;
            }

            auto *body = dynamic_cast<AstStatList *>(structDecl->body);
            if (!body) {
                continue;
            }

            llvm::StringMap<StructType::ValueTy> members;
            int index = 0;
            for (auto *stmt : body->getBody()) {
                auto *varDecl = dynamic_cast<AstVarDecl *>(stmt);
                if (!varDecl) {
                    continue;
                }
                if (varDecl->bindingKind == BindingKind::Ref) {
                    error(varDecl->loc,
                          "struct fields cannot use `ref` binding for `" +
                              toStdString(varDecl->field) + "`",
                          "Store an explicit pointer type instead. Struct fields must be value or pointer-like storage.");
                }
                auto *fieldType = resolveType(varDecl->typeNode);
                if (!fieldType) {
                    error(varDecl->loc,
                          "unknown struct field type for `" +
                              toStdString(varDecl->field) + "`: " +
                              describeTypeNode(varDecl->typeNode, "void"));
                }
                rejectBareFunctionType(
                    fieldType, varDecl->typeNode,
                    "unsupported bare function struct field type for `" +
                        toStdString(varDecl->field) + "`",
                    varDecl->loc);
                validateStructFieldType(structDecl, varDecl, fieldType);
                members.insert({llvm::StringRef(varDecl->field.tochara(),
                                                varDecl->field.size()),
                                {fieldType, index++}});
            }

            structType->complete(members);
        }
    }

    FuncType *buildFunctionType(AstFuncDecl *node, StructType *methodParent) {
        std::vector<TypeClass *> argTypes;
        auto argBindingKinds = extractParamBindingKinds(node, methodParent != nullptr);
        if (methodParent) {
            argTypes.push_back(methodParent);
        }
        if (node->args) {
            for (auto *arg : *node->args) {
                auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
                if (!varDecl) {
                    error(node->loc,
                          "invalid function parameter declaration in `" +
                              toStdString(node->name) + "`");
                }
                auto *argType = resolveType(varDecl->typeNode);
                if (!argType) {
                    error(varDecl->loc,
                          "unknown type for function parameter `" +
                              toStdString(varDecl->field) + "` in `" +
                              toStdString(node->name) + "`: " +
                              describeTypeNode(varDecl->typeNode, "void"));
                }
                rejectBareFunctionType(
                    argType, varDecl->typeNode,
                    "unsupported bare function parameter type for `" +
                        toStdString(varDecl->field) + "` in `" +
                        toStdString(node->name) + "`",
                    varDecl->loc);
                rejectOpaqueStructByValue(
                    argType, varDecl->typeNode, varDecl->loc,
                    "parameter `" + toStdString(varDecl->field) +
                        "` in function `" + toStdString(node->name) + "`");
                argTypes.push_back(argType);
            }
        }

        TypeClass *retType = nullptr;
        if (node->retType) {
            retType = resolveType(node->retType);
            if (!retType) {
                error(node->loc,
                      "unknown return type for function `" +
                          toStdString(node->name) + "`: " +
                          describeTypeNode(node->retType, "void"));
            }
            rejectBareFunctionType(
                retType, node->retType,
                "unsupported bare function return type for `" +
                    toStdString(node->name) + "`",
                node->loc);
            rejectOpaqueStructByValue(
                retType, node->retType, node->loc,
                "return type of function `" + toStdString(node->name) + "`");
        }

        validateExternCFunctionSignature(node, methodParent, argTypes, retType);
        return interface_->getOrCreateFunctionType(argTypes, retType,
                                                   std::move(argBindingKinds),
                                                   node->abiKind);
    }

    void declareFunctions() {
        for (auto *structDecl : structDecls_) {
            auto *typeDecl = interface_->findType(toStdString(structDecl->name));
            auto *structType = typeDecl ? typeDecl->type->as<StructType>() : nullptr;
            auto *body = dynamic_cast<AstStatList *>(structDecl->body);
            if (!structType || !body) {
                continue;
            }
            for (auto *stmt : body->getBody()) {
                auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt);
                if (!funcDecl) {
                    continue;
                }
                auto *funcType = buildFunctionType(funcDecl, structType);
                if (structType->getMethodType(
                        llvm::StringRef(funcDecl->name.tochara(), funcDecl->name.size())) ==
                    nullptr) {
                    structType->addMethodType(
                        llvm::StringRef(funcDecl->name.tochara(), funcDecl->name.size()),
                        funcType, extractParamNames(funcDecl));
                }
            }
        }

        for (auto *funcDecl : funcDecls_) {
            auto *funcType = buildFunctionType(funcDecl, nullptr);
            interface_->declareFunction(toStdString(funcDecl->name), funcType,
                                        extractParamNames(funcDecl));
        }
    }

public:
    explicit InterfaceCollector(CompilationUnit &unit)
        : unit_(unit), interface_(unit.interface()) {
        assert(interface_);
    }

    void collect() {
        interface_->clear();
        collectTopLevelLists(unit_.syntaxTree());
        declareStructs();
        completeStructs();
        declareFunctions();
        interface_->markCollected();
    }
};

void
ensureUnitInterfaceCollected(CompilationUnit &unit) {
    if (unit.interfaceCollected()) {
        return;
    }
    InterfaceCollector(unit).collect();
}

Function *
materializeDeclaredFunction(Scope &scope, TypeTable *typeMgr, FuncType *funcType,
                            llvm::StringRef llvmName,
                            std::vector<std::string> paramNames = {},
                            bool hasImplicitSelf = false) {
    auto *existing = scope.getObj(llvmName);
    if (existing) {
        return existing->as<Function>();
    }
    auto *llvmFunc = llvm::Function::Create(
                                            getFunctionAbiLLVMType(
                                                *typeMgr, funcType, hasImplicitSelf),
                                            llvm::Function::ExternalLinkage,
                                            llvm::Twine(llvmName),
                                            typeMgr->getModule());
    auto *func = new Function(llvmFunc, funcType, std::move(paramNames),
                              hasImplicitSelf);
    scope.addObj(llvmName, func);
    return func;
}

void
materializeUnitInterface(Scope *global, CompilationUnit &unit, bool exportNamespace) {
    initBuildinType(global);
    ensureUnitInterfaceCollected(unit);
    auto *interface = unit.interface();
    auto *typeMgr = requireTypeTable(global);
    unit.clearLocalBindings();

    if (exportNamespace) {
        declareModuleNamespace(*global, unit);
    }

    for (const auto &entry : interface->types()) {
        auto *type = typeMgr->internType(entry.second.type);
        if (!type) {
            internalError("failed to materialize imported type `" + entry.first +
                              "` from module `" + unit.path() + "`",
                          "Imported interfaces should only contain types that were "
                          "successfully collected from the defining module.");
        }
        unit.bindLocalType(entry.first, toStdString(type->full_name));
    }

    for (const auto &entry : interface->functions()) {
        auto *funcType = typeMgr->internType(entry.second.type)->as<FuncType>();
        if (!funcType) {
            internalError(
                "failed to materialize imported function signature `" + entry.first +
                    "` from module `" + unit.path() + "`",
                "Imported interfaces should only contain function signatures that "
                "were successfully collected from the defining module.");
        }
        auto runtimeName = exportNamespace ? entry.second.symbolName : entry.first;
        unit.bindLocalFunction(entry.first, runtimeName);
        materializeDeclaredFunction(*global, typeMgr, funcType,
                                    llvm::StringRef(runtimeName),
                                    entry.second.paramNames);
    }

    for (const auto &entry : interface->types()) {
        auto *structType = typeMgr->internType(entry.second.type)->as<StructType>();
        if (!structType) {
            continue;
        }
        for (const auto &method : structType->getMethodTypes()) {
            auto *methodType = typeMgr->internType(method.second)->as<FuncType>();
            if (!methodType) {
                continue;
            }
            if (typeMgr->getMethodFunction(structType, method.first())) {
                continue;
            }
            auto methodName = toStdString(structType->full_name) + "." + method.first().str();
            auto *llvmFunc = llvm::Function::Create(
                                                    getFunctionAbiLLVMType(
                                                        *typeMgr, methodType, true),
                                                    llvm::Function::ExternalLinkage,
                                                    llvm::Twine(methodName),
                                                    typeMgr->getModule());
            std::vector<std::string> paramNames;
            if (const auto *storedParamNames =
                    structType->getMethodParamNames(method.first())) {
                paramNames = *storedParamNames;
            }
            typeMgr->bindMethodFunction(
                structType, method.first(),
                new Function(llvmFunc, methodType, std::move(paramNames), true));
        }
    }

    unit.markInterfaceCollected();
}

}  // namespace

Function *
createFunc(Scope &scope, AstFuncDecl *root, StructType *parent) {
    initBuildinType(&scope);
    auto *func =
        declareFunction(scope, requireTypeTable(&scope), root, parent, nullptr, false);
    return func;
}

void
scanningType(Scope *global, AstNode *root) {
    initBuildinType(global);
    TypeCollector(requireTypeTable(global), global, root, nullptr, false);
}

void
collectUnitDeclarations(Scope *global, CompilationUnit &unit, bool exportNamespace) {
    materializeUnitInterface(global, unit, exportNamespace);
}

StructType *
createStruct(Scope *scope, AstStructDecl *node) {
    initBuildinType(scope);
    auto *typeMgr = requireTypeTable(scope);
    auto *type = declareStructType(typeMgr, node, nullptr, false);
    if (type->isOpaque()) {
        StructVisitor(typeMgr, node, nullptr, false);
    }
    return type;
}

namespace {

FuncType *
getFunctionPointerTarget(TypeClass *type) {
    auto *pointerType = asUnqualified<PointerType>(type);
    return pointerType ? pointerType->getPointeeType()->as<FuncType>() : nullptr;
}

Function *
getDirectFunctionCallee(HIRExpr *callee) {
    auto *calleeValue = dynamic_cast<HIRValue *>(callee);
    auto *value = calleeValue ? calleeValue->getValue() : nullptr;
    return value ? value->as<Function>() : nullptr;
}

class FunctionCompiler {
    struct LoopContext {
        llvm::BasicBlock *continueBlock = nullptr;
        llvm::BasicBlock *breakBlock = nullptr;
    };

    TypeTable *typeMgr;
    GlobalScope *global;
    FuncScope *scope;
    llvm::LLVMContext &context;
    DebugInfoContext *debug;
    llvm::DISubprogram *debugSubprogram = nullptr;
    AbiFunctionSignature abiSignature;
    bool returnByPointer = false;
    location currentLocation;
    bool hasCurrentLocation = false;
    std::vector<LoopContext> loopStack;

    [[noreturn]] void error(const std::string &message) {
        if (hasCurrentLocation) {
            lona::error(currentLocation, message);
        }
        lona::error(message);
    }

    [[noreturn]] void error(const std::string &message, const std::string &hint) {
        if (hasCurrentLocation) {
            lona::error(currentLocation, message, hint);
        }
        throw DiagnosticError(DiagnosticError::Category::Semantic, message, hint);
    }

    [[noreturn]] void error(const location &loc, const std::string &message,
                            const std::string &hint = std::string()) {
        lona::error(loc, message, hint);
    }

    [[noreturn]] void functionError(HIRFunc *hirFunc, const std::string &message,
                                    const std::string &hint = std::string()) {
        if (hirFunc) {
            error(hirFunc->getLocation(), message, hint);
        }
        error(message, hint);
    }

    llvm::Constant *buildByteStringArrayConstant(const std::string &bytes) {
        std::vector<std::uint8_t> data;
        data.reserve(bytes.size());
        for (unsigned char byte : bytes) {
            data.push_back(static_cast<std::uint8_t>(byte));
        }
        return llvm::ConstantDataArray::get(context, data);
    }

    std::string nextByteStringGlobalName() {
        static std::uint64_t nextId = 0;
        return ".lona.bytes." + std::to_string(nextId++);
    }

    llvm::GlobalVariable *createByteStringGlobal(const std::string &bytes) {
        auto *initializer = buildByteStringArrayConstant(bytes);
        auto *llvmArrayType =
            llvm::cast<llvm::ArrayType>(initializer->getType());
        auto *globalValue = new llvm::GlobalVariable(
            scope->module, llvmArrayType, true,
            llvm::GlobalValue::PrivateLinkage, initializer,
            nextByteStringGlobalName());
        globalValue->setAlignment(llvm::MaybeAlign(1));
        return globalValue;
    }

    Object *materializeLocal(TypeClass *type, Object *initVal) {
        auto *obj = type->newObj(Object::VARIABLE);
        obj->createllvmValue(scope);
        if (initVal != nullptr) {
            obj->set(scope, initVal);
        }
        return obj;
    }

    Object *materializeBinding(Object *obj, Object *initVal = nullptr) {
        if (!obj) {
            error("missing binding object");
        }
        if (obj->isRefAlias()) {
            if (initVal == nullptr) {
                error("reference binding requires an addressable source");
            }
            if (!initVal->isVariable() || initVal->isRegVal() ||
                !initVal->getllvmValue()) {
                error("reference binding expects an addressable source");
            }
            if (!isConstQualificationConvertible(obj->getType(),
                                                 initVal->getType())) {
                error("reference binding type mismatch during lowering");
            }
            obj->setllvmValue(initVal->getllvmValue());
            return obj;
        }
        obj->createllvmValue(scope);
        if (initVal != nullptr) {
            obj->set(scope, initVal);
        }
        return obj;
    }

    Object *materializeIndirectValueBinding(Object *obj, llvm::Value *incomingPtr) {
        if (!obj || !incomingPtr) {
            error("indirect value binding requires an incoming pointer");
        }
        auto *bound = materializeBinding(obj);
        auto *value = scope->builder.CreateLoad(scope->getLLVMType(obj->getType()),
                                                incomingPtr);
        scope->builder.CreateStore(value, bound->getllvmValue());
        return bound;
    }

    Object *materializeDirectValueBinding(Object *obj, llvm::Value *incomingValue,
                                          bool packedRegisterAggregate) {
        if (!obj || !incomingValue) {
            error("direct value binding requires an incoming value");
        }
        auto *bound = materializeBinding(obj);
        if (packedRegisterAggregate) {
            storeNativeAbiDirectValue(scope->builder, *typeMgr, obj->getType(),
                                      incomingValue, bound->getllvmValue());
        } else {
            scope->builder.CreateStore(incomingValue, bound->getllvmValue());
        }
        return bound;
    }

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

    void setLocation(const location &loc) {
        currentLocation = loc;
        hasCurrentLocation = true;
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

    const LoopContext &requireCurrentLoop() {
        if (loopStack.empty()) {
            error("loop control statement escaped semantic validation");
        }
        return loopStack.back();
    }

    Object *compileExpr(HIRExpr *expr) {
        if (!expr) {
            return nullptr;
        }
        if (auto *value = dynamic_cast<HIRValue *>(expr)) {
            return value->getValue();
        }
        if (auto *tuple = dynamic_cast<HIRTupleLiteral *>(expr)) {
            setLocation(tuple);
            auto *tupleType = asUnqualified<TupleType>(tuple->getType());
            if (!tupleType) {
                error("tuple literal is missing its tuple type");
            }
            auto *llvmTupleType = typeMgr->getLLVMType(tupleType);
            llvm::Value *aggregate = llvm::UndefValue::get(llvmTupleType);
            const auto &itemTypes = tupleType->getItemTypes();
            if (itemTypes.size() != tuple->getItems().size()) {
                error("tuple literal item count mismatch during lowering");
            }
            for (size_t i = 0; i < tuple->getItems().size(); ++i) {
                auto *item = compileExpr(tuple->getItems()[i]);
                if (!item) {
                    error("tuple literal item did not produce a value");
                }
                auto *itemValue = item->get(scope);
                if (!isByteCopyCompatible(itemTypes[i], item->getType())) {
                    error("tuple literal item type mismatch during lowering");
                }
                aggregate = scope->builder.CreateInsertValue(
                    aggregate, itemValue, {static_cast<unsigned>(i)});
            }
            auto *result = tupleType->newObj(Object::REG_VAL | Object::READONLY);
            result->bindllvmValue(aggregate);
            return result;
        }
        if (auto *structLiteral = dynamic_cast<HIRStructLiteral *>(expr)) {
            setLocation(structLiteral);
            auto *structType = asUnqualified<StructType>(structLiteral->getType());
            if (!structType) {
                error("struct literal is missing its struct type");
            }
            auto *llvmStructType =
                llvm::dyn_cast<llvm::StructType>(typeMgr->getLLVMType(structType));
            if (!llvmStructType) {
                error("struct literal lowering requires an LLVM struct type");
            }

            std::vector<std::pair<TypeClass *, int>> orderedMembers(
                structType->getMembers().size(), {nullptr, -1});
            for (const auto &member : structType->getMembers()) {
                const auto index = static_cast<size_t>(member.second.second);
                if (index >= orderedMembers.size()) {
                    error("struct literal member index is out of range");
                }
                orderedMembers[index] = member.second;
            }
            if (orderedMembers.size() != structLiteral->getFields().size()) {
                error("struct literal field count mismatch during lowering");
            }

            llvm::Value *aggregate = llvm::UndefValue::get(llvmStructType);
            for (size_t i = 0; i < structLiteral->getFields().size(); ++i) {
                auto *field = compileExpr(structLiteral->getFields()[i]);
                if (!field) {
                    error("struct literal field did not produce a value");
                }
                auto *fieldType = orderedMembers[i].first;
                if (!isByteCopyCompatible(fieldType, field->getType())) {
                    error("struct literal field type mismatch during lowering");
                }
                aggregate = scope->builder.CreateInsertValue(
                    aggregate, field->get(scope), {static_cast<unsigned>(i)});
            }
            auto *result = structType->newObj(Object::REG_VAL | Object::READONLY);
            result->bindllvmValue(aggregate);
            return result;
        }
        if (auto *arrayInit = dynamic_cast<HIRArrayInit *>(expr)) {
            setLocation(arrayInit);
            auto *arrayType = asUnqualified<ArrayType>(arrayInit->getType());
            if (!arrayType || !arrayType->hasStaticLayout()) {
                error("array initializer requires a fixed-layout array type");
            }
            auto *childType = arrayInitChildType(arrayType);
            if (!childType) {
                error("array initializer is missing its child element type");
            }
            llvm::Value *aggregate = llvm::Constant::getNullValue(
                scope->getLLVMType(arrayType));
            for (std::size_t i = 0; i < arrayInit->getItems().size(); ++i) {
                auto *item = compileExpr(arrayInit->getItems()[i]);
                if (!item) {
                    error("array initializer item did not produce a value");
                }
                if (!isByteCopyCompatible(childType, item->getType())) {
                    error("array initializer item type mismatch during lowering");
                }
                aggregate = scope->builder.CreateInsertValue(
                    aggregate, item->get(scope), {static_cast<unsigned>(i)});
            }
            auto *result = arrayType->newObj(Object::REG_VAL | Object::READONLY);
            result->bindllvmValue(aggregate);
            return result;
        }
        if (auto *byteString = dynamic_cast<HIRByteStringLiteral *>(expr)) {
            setLocation(byteString);
            if (!byteString->isBorrowed()) {
                auto *globalValue = createByteStringGlobal(byteString->getBytes());
                auto *result = byteString->getType()->newObj(
                    Object::VARIABLE | Object::READONLY);
                result->setllvmValue(globalValue);
                return result;
            }

            auto *globalValue = createByteStringGlobal(byteString->getBytes());
            auto *llvmArrayType =
                llvm::cast<llvm::ArrayType>(globalValue->getValueType());
            auto *zero = llvm::ConstantInt::get(scope->builder.getInt32Ty(), 0, true);
            auto *borrowed = scope->builder.CreateInBoundsGEP(
                llvmArrayType, globalValue, {zero, zero});
            return makeReadonlyValue(byteString->getType(), borrowed);
        }
        if (auto *nullLiteral = dynamic_cast<HIRNullLiteral *>(expr)) {
            setLocation(nullLiteral);
            auto *type = nullLiteral->getType();
            if (!isPointerLikeType(type)) {
                error("null literal requires a concrete pointer type");
            }
            auto *value = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(scope->getLLVMType(type)));
            return makeReadonlyValue(type, value);
        }
        if (auto *cast = dynamic_cast<HIRNumericCast *>(expr)) {
            setLocation(cast);
            return emitNumericCast(cast);
        }
        if (auto *bitCast = dynamic_cast<HIRBitCast *>(expr)) {
            setLocation(bitCast);
            return emitBitCopyCast(bitCast);
        }
        if (auto *assign = dynamic_cast<HIRAssign *>(expr)) {
            setLocation(assign);
            auto *dst = compileExpr(assign->getLeft());
            auto *src = compileExpr(assign->getRight());
            if (!dst || !src) {
                error("assignment requires values");
            }
            dst->set(scope, src);
            return dst;
        }
        if (auto *bin = dynamic_cast<HIRBinOper *>(expr)) {
            setLocation(bin);
            if (bin->getBinding().shortCircuit) {
                return emitShortCircuitBinary(bin);
            }
            auto *left = compileExpr(bin->getLeft());
            auto *right = compileExpr(bin->getRight());
            return emitBinaryOperator(bin->getBinding(), left, right);
        }
        if (auto *unary = dynamic_cast<HIRUnaryOper *>(expr)) {
            setLocation(unary);
            auto *value = compileExpr(unary->getExpr());
            return emitUnaryOperator(unary->getBinding(), value);
        }
        if (auto *selector = dynamic_cast<HIRSelector *>(expr)) {
            setLocation(selector);
            auto *parent = compileExpr(selector->getParent());
            auto fieldName = selector->getFieldName();
            if (auto *tupleParent = parent->as<TupleVar>()) {
                if (!selector->isValueFieldSelector()) {
                    error("tuple selectors do not support method calls");
                }
                return tupleParent->getField(scope, fieldName);
            }
            if (auto *structParent = parent->as<StructVar>()) {
                if (selector->isMethodSelector()) {
                    error(kMethodSelectorDirectCallError);
                }
                return structParent->getField(scope, fieldName);
            }
            error("selector parent must be a struct or tuple value");
        }
        if (auto *call = dynamic_cast<HIRCall *>(expr)) {
            setLocation(call);
            std::vector<Object *> args;
            llvm::Value *calleeValue = nullptr;
            FuncType *funcType = nullptr;
            bool hasImplicitSelf = false;

            if (auto *selector = dynamic_cast<HIRSelector *>(call->getCallee());
                selector && selector->isMethodSelector()) {
                auto *parent = compileExpr(selector->getParent());
                auto *structType = asUnqualified<StructType>(parent->getType());
                if (!structType) {
                    error("selector call parent must be a struct value");
                }
                auto *callee = scope->getMethodFunction(
                    structType, llvm::StringRef(selector->getFieldName()));
                if (!callee) {
                    error("unknown struct method");
                }
                funcType = callee->getType()->as<FuncType>();
                calleeValue = callee->getllvmValue();
                if (funcType && !funcType->getArgTypes().empty() &&
                    funcType->getArgBindingKind(0) == BindingKind::Ref &&
                    (!parent->isVariable() || parent->isRegVal() ||
                     !parent->getllvmValue())) {
                    parent = materializeLocal(parent->getType(), parent);
                }
                args.push_back(parent);
                hasImplicitSelf = true;
            } else if (auto *callee = getDirectFunctionCallee(call->getCallee())) {
                funcType = callee->getType()->as<FuncType>();
                calleeValue = callee->getllvmValue();
                hasImplicitSelf = callee->hasImplicitSelf();
            } else {
                auto *calleeObj = compileExpr(call->getCallee());
                if (!calleeObj) {
                    error("call target did not produce a value");
                }
                funcType = getFunctionPointerTarget(calleeObj->getType());
                if (!funcType) {
                    error("callee must be a function, function pointer, or method selector");
                }
                calleeValue = calleeObj->get(scope);
            }

            args.reserve(args.size() + call->getArgs().size());
            for (auto *arg : call->getArgs()) {
                auto *value = compileExpr(arg);
                if (!value) {
                    error("call argument did not produce a value");
                }
                args.push_back(value);
            }
            return emitFunctionCall(scope, calleeValue, funcType, args,
                                    hasImplicitSelf);
        }
        if (auto *index = dynamic_cast<HIRIndex *>(expr)) {
            setLocation(index);
            auto *target = compileExpr(index->getTarget());
            if (!target) {
                error("array indexing target did not produce a value");
            }
            auto *arrayType = asUnqualified<ArrayType>(target->getType());
            auto *indexableType =
                asUnqualified<IndexablePointerType>(target->getType());
            if (!arrayType && !indexableType) {
                error("array indexing expects an array value or indexable pointer");
            }

            std::vector<llvm::Value *> gepIndices;
            llvm::Type *gepSourceType = nullptr;
            llvm::Value *targetPtr = nullptr;
            const bool fixedLayout = arrayType && arrayType->hasStaticLayout();
            if (arrayType && !fixedLayout) {
                error("array indexing requires a fixed-layout array type or an indexable pointer");
            }
            gepIndices.reserve(index->getIndices().size() + (fixedLayout ? 1 : 0));
            if (fixedLayout) {
                targetPtr = target->getllvmValue();
                if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
                    error("array indexing expects an addressable array value");
                }
                gepSourceType = scope->getLLVMType(arrayType);
                gepIndices.push_back(
                    llvm::ConstantInt::get(scope->builder.getInt32Ty(), 0, true));
            } else {
                targetPtr = target->get(scope);
                if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
                    error("array indexing expects a pointer value");
                }
                gepSourceType = scope->getLLVMType(indexableType->getElementType());
            }
            for (auto *argExpr : index->getIndices()) {
                auto *arg = compileExpr(argExpr);
                if (!arg || arg->getType() != i32Ty) {
                    error("array indexing expects `i32` indices");
                }
                gepIndices.push_back(arg->get(scope));
            }

            auto *resultType = index->getType();
            if (!resultType) {
                error("array indexing result type is missing");
            }
            auto *elementPtr = scope->builder.CreateInBoundsGEP(
                gepSourceType, targetPtr, gepIndices);
            auto *result = resultType->newObj(Object::VARIABLE);
            result->setllvmValue(elementPtr);
            return result;
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
                    error(ret->getLocation(), "unexpected return value in void function");
                }
                retSlot->set(scope, value);
            } else if (retSlot) {
                error(ret->getLocation(), "missing return value");
            }

            if (scope->retBlock()) {
                scope->builder.CreateBr(scope->retBlock());
            } else if (retSlot) {
                scope->builder.CreateRet(retSlot->get(scope));
            } else {
                scope->builder.CreateRetVoid();
            }
            scope->setReturned();
            return retSlot;
        }
        if (auto *breakNode = dynamic_cast<HIRBreak *>(node)) {
            setLocation(breakNode);
            scope->builder.CreateBr(requireCurrentLoop().breakBlock);
            return nullptr;
        }
        if (auto *continueNode = dynamic_cast<HIRContinue *>(node)) {
            setLocation(continueNode);
            scope->builder.CreateBr(requireCurrentLoop().continueBlock);
            return nullptr;
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
            auto *elseBB = forNode->hasElseBlock()
                ? llvm::BasicBlock::Create(context, "for.else")
                : endBB;

            scope->builder.CreateBr(condBB);

            scope->builder.SetInsertPoint(condBB);
            auto *condObj = compileExpr(forNode->getCondition());
            scope->builder.CreateCondBr(emitBoolCast(condObj), bodyBB, elseBB);

            llvmFunc->insert(llvmFunc->end(), bodyBB);
            scope->builder.SetInsertPoint(bodyBB);
            loopStack.push_back({condBB, endBB});
            compileBlock(forNode->getBody());
            loopStack.pop_back();
            if (!scope->builder.GetInsertBlock()->getTerminator()) {
                scope->builder.CreateBr(condBB);
            }

            if (forNode->hasElseBlock()) {
                llvmFunc->insert(llvmFunc->end(), elseBB);
                scope->builder.SetInsertPoint(elseBB);
                compileBlock(forNode->getElseBlock());
                if (!scope->builder.GetInsertBlock()->getTerminator()) {
                    scope->builder.CreateBr(endBB);
                }
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
        auto *value = obj->get(scope);
        auto *type = obj->getType();
        if (isBoolStorageType(type)) {
            return scope->builder.CreateICmpNE(
                value, llvm::ConstantInt::get(scope->getLLVMType(type), 0));
        }
        if (isIntegerType(type)) {
            return scope->builder.CreateICmpNE(
                value, llvm::ConstantInt::get(scope->getLLVMType(type), 0));
        }
        if (type == f32Ty) {
            return scope->builder.CreateFCmpUNE(
                value, llvm::ConstantFP::get(scope->getLLVMType(f32Ty), 0.0f));
        }
        if (type == f64Ty) {
            return scope->builder.CreateFCmpUNE(
                value, llvm::ConstantFP::get(scope->getLLVMType(f64Ty), 0.0));
        }
        if (isPointerLikeType(type)) {
            return scope->builder.CreateICmpNE(
                value, llvm::ConstantPointerNull::get(
                           llvm::cast<llvm::PointerType>(value->getType())));
        }
        error("unsupported condition type");
    }

    Object *makeReadonlyValue(TypeClass *type, llvm::Value *value) {
        if (isBoolStorageType(type) && value) {
            auto *boolLLVMType = scope->getLLVMType(boolTy);
            if (value->getType() != boolLLVMType) {
                if (value->getType()->isIntegerTy(1)) {
                    value = scope->builder.CreateZExt(value, boolLLVMType);
                } else if (value->getType()->isIntegerTy()) {
                    auto *isTrue = scope->builder.CreateICmpNE(
                        value, llvm::ConstantInt::get(value->getType(), 0));
                    value = scope->builder.CreateZExt(isTrue, boolLLVMType);
                } else {
                    error("bool value lowering expects an integer LLVM value");
                }
            }
        }
        auto *obj = type->newObj(Object::REG_VAL | Object::READONLY);
        obj->bindllvmValue(value);
        return obj;
    }

    Object *emitNumericCast(HIRNumericCast *cast) {
        auto *source = compileExpr(cast->getExpr());
        if (!source || !source->getType() || !cast->getType()) {
            error("numeric cast requires concrete source and target types");
        }

        auto *sourceType = source->getType();
        auto *targetType = cast->getType();
        auto *value = source->get(scope);
        if (sourceType == targetType) {
            return makeReadonlyValue(targetType, value);
        }

        llvm::Value *result = nullptr;
        if (isIntegerType(sourceType) && isIntegerType(targetType)) {
            auto sourceBits = static_cast<unsigned>(scope->types()->getTypeAllocSize(sourceType) * 8);
            auto targetBits = static_cast<unsigned>(scope->types()->getTypeAllocSize(targetType) * 8);
            if (sourceBits == targetBits) {
                result = value;
            } else if (sourceBits < targetBits) {
                result = isSignedIntegerType(sourceType)
                    ? scope->builder.CreateSExt(value, scope->getLLVMType(targetType))
                    : scope->builder.CreateZExt(value, scope->getLLVMType(targetType));
            } else {
                result = scope->builder.CreateTrunc(value, scope->getLLVMType(targetType));
            }
        } else if (isFloatType(sourceType) && isFloatType(targetType)) {
            auto sourceBits = static_cast<unsigned>(scope->types()->getTypeAllocSize(sourceType) * 8);
            auto targetBits = static_cast<unsigned>(scope->types()->getTypeAllocSize(targetType) * 8);
            if (sourceBits == targetBits) {
                result = value;
            } else if (sourceBits < targetBits) {
                result = scope->builder.CreateFPExt(value, scope->getLLVMType(targetType));
            } else {
                result = scope->builder.CreateFPTrunc(value, scope->getLLVMType(targetType));
            }
        } else if (isIntegerType(sourceType) && isFloatType(targetType)) {
            result = isSignedIntegerType(sourceType)
                ? scope->builder.CreateSIToFP(value, scope->getLLVMType(targetType))
                : scope->builder.CreateUIToFP(value, scope->getLLVMType(targetType));
        } else if (isFloatType(sourceType) && isIntegerType(targetType)) {
            result = isSignedIntegerType(targetType)
                ? scope->builder.CreateFPToSI(value, scope->getLLVMType(targetType))
                : scope->builder.CreateFPToUI(value, scope->getLLVMType(targetType));
        } else {
            error("unsupported numeric cast");
        }

        return makeReadonlyValue(targetType, result);
    }

    Object *emitBitCopyCast(HIRBitCast *cast) {
        auto *source = compileExpr(cast->getExpr());
        if (!source || !source->getType() || !cast->getType()) {
            error("bit-copy cast requires concrete source and target types");
        }

        auto *sourceType = source->getType();
        auto *targetType = cast->getType();
        auto *sourceValue = source->get(scope);
        if (sourceType == targetType) {
            return makeReadonlyValue(targetType, sourceValue);
        }

        auto bitWidthFor = [this](TypeClass *type) -> unsigned {
            auto byteSize = scope->types()->getTypeAllocSize(type);
            if (byteSize == 0) {
                error("bit-copy cast requires a concrete data layout size");
            }
            return static_cast<unsigned>(byteSize * 8);
        };

        auto targetBitsWidth = bitWidthFor(targetType);
        auto *targetBitsType =
            llvm::IntegerType::get(scope->builder.getContext(), targetBitsWidth);

        auto isBitsArray = [](TypeClass *type) {
            auto *array = asUnqualified<ArrayType>(type);
            return array && array->getElementType() == u8Ty && array->hasStaticLayout() &&
                array->staticDimensions().size() == 1;
        };

        llvm::Value *bits = nullptr;
        unsigned bitsWidth = 0;
        if (sourceValue->getType()->isIntegerTy()) {
            bitsWidth = bitWidthFor(sourceType);
            auto *sourceBitsType =
                llvm::IntegerType::get(scope->builder.getContext(), bitsWidth);
            bits = sourceValue;
            if (bits->getType() != sourceBitsType) {
                bits = scope->builder.CreateTruncOrBitCast(bits, sourceBitsType);
            }
        } else if (sourceValue->getType()->isFloatingPointTy()) {
            bitsWidth = bitWidthFor(sourceType);
            auto *sourceBitsType =
                llvm::IntegerType::get(scope->builder.getContext(), bitsWidth);
            bits = scope->builder.CreateBitCast(sourceValue, sourceBitsType);
        } else if (sourceValue->getType()->isPointerTy()) {
            bitsWidth = bitWidthFor(sourceType);
            auto *sourceBitsType =
                llvm::IntegerType::get(scope->builder.getContext(), bitsWidth);
            bits = scope->builder.CreatePtrToInt(sourceValue, sourceBitsType);
        } else if (isBitsArray(sourceType)) {
            auto dims = asUnqualified<ArrayType>(sourceType)->staticDimensions();
            const auto relevantBytes =
                std::max<std::int64_t>(1, std::min<std::int64_t>(
                                              dims[0],
                                              static_cast<std::int64_t>(
                                                  scope->types()->getTypeAllocSize(targetType))));
            bitsWidth = static_cast<unsigned>(relevantBytes * 8);
            auto *sourceBitsType =
                llvm::IntegerType::get(scope->builder.getContext(), bitsWidth);
            bits = llvm::ConstantInt::get(sourceBitsType, 0);
            for (std::int64_t i = 0; i < relevantBytes; ++i) {
                auto *byteValue =
                    scope->builder.CreateExtractValue(sourceValue, {static_cast<unsigned>(i)});
                auto *byteBits = scope->builder.CreateZExt(byteValue, sourceBitsType);
                if (i != 0) {
                    byteBits = scope->builder.CreateShl(
                        byteBits, llvm::ConstantInt::get(sourceBitsType, i * 8));
                }
                bits = scope->builder.CreateOr(bits, byteBits);
            }
        } else {
            error("unsupported source type for raw bit-copy");
        }

        if (bitsWidth < targetBitsWidth) {
            bits = scope->builder.CreateZExt(bits, targetBitsType);
        } else if (bitsWidth > targetBitsWidth) {
            bits = scope->builder.CreateTrunc(bits, targetBitsType);
        }

        auto *targetLLVMType = scope->getLLVMType(targetType);
        llvm::Value *result = nullptr;
        if (targetLLVMType->isIntegerTy()) {
            result = bits;
        } else if (targetLLVMType->isFloatingPointTy()) {
            result = scope->builder.CreateBitCast(bits, targetLLVMType);
        } else if (targetLLVMType->isPointerTy()) {
            result = scope->builder.CreateIntToPtr(bits, targetLLVMType);
        } else if (isBitsArray(targetType)) {
            result = llvm::UndefValue::get(targetLLVMType);
            auto dims = asUnqualified<ArrayType>(targetType)->staticDimensions();
            for (std::int64_t i = 0; i < dims[0]; ++i) {
                auto *shift =
                    i == 0 ? bits : scope->builder.CreateLShr(
                                         bits, llvm::ConstantInt::get(targetBitsType, i * 8));
                auto *byteValue = scope->builder.CreateTrunc(shift, scope->getLLVMType(u8Ty));
                result = scope->builder.CreateInsertValue(
                    result, byteValue, {static_cast<unsigned>(i)});
            }
        } else {
            error("unsupported target type for raw bit-copy");
        }

        return makeReadonlyValue(targetType, result);
    }

    Object *emitUnaryOperator(const UnaryOperatorBinding &binding, Object *value) {
        auto *llvmValue = binding.kind == UnaryOperatorKind::AddressOf
            ? value->getllvmValue()
            : (binding.kind == UnaryOperatorKind::Dereference ? value->get(scope)
                                                              : value->get(scope));

        switch (binding.kind) {
        case UnaryOperatorKind::Identity:
            return value;
        case UnaryOperatorKind::Negate:
            return makeReadonlyValue(
                binding.resultType,
                binding.operandClass == OperatorOperandClass::Float
                    ? scope->builder.CreateFNeg(llvmValue)
                    : scope->builder.CreateNeg(llvmValue));
        case UnaryOperatorKind::LogicalNot:
            return makeReadonlyValue(boolTy, scope->builder.CreateNot(emitBoolCast(value)));
        case UnaryOperatorKind::BitwiseNot:
            return makeReadonlyValue(binding.resultType,
                                     scope->builder.CreateNot(llvmValue));
        case UnaryOperatorKind::AddressOf:
            if (!value->isVariable() || value->isRegVal() || !llvmValue) {
                error("address-of expects an addressable value");
            }
            return makeReadonlyValue(binding.resultType, llvmValue);
        case UnaryOperatorKind::Dereference: {
            auto *result = binding.resultType->newObj(Object::VARIABLE);
            result->setllvmValue(llvmValue);
            return result;
        }
        default:
            error("unsupported unary operator binding");
        }
    }

    Object *emitBinaryOperator(const BinaryOperatorBinding &binding, Object *left,
                               Object *right) {
        auto *lhs = left->get(scope);
        auto *rhs = right->get(scope);
        if (binding.leftClass == OperatorOperandClass::Bool &&
            binding.rightClass == OperatorOperandClass::Bool &&
            (binding.kind == BinaryOperatorKind::BitAnd ||
             binding.kind == BinaryOperatorKind::BitXor ||
             binding.kind == BinaryOperatorKind::BitOr)) {
            lhs = emitBoolCast(left);
            rhs = emitBoolCast(right);
        }
        llvm::Value *result = nullptr;

        switch (binding.kind) {
        case BinaryOperatorKind::Add:
            result = binding.leftClass == OperatorOperandClass::Float
                ? scope->builder.CreateFAdd(lhs, rhs)
                : scope->builder.CreateAdd(lhs, rhs);
            break;
        case BinaryOperatorKind::Sub:
            result = binding.leftClass == OperatorOperandClass::Float
                ? scope->builder.CreateFSub(lhs, rhs)
                : scope->builder.CreateSub(lhs, rhs);
            break;
        case BinaryOperatorKind::Mul:
            result = binding.leftClass == OperatorOperandClass::Float
                ? scope->builder.CreateFMul(lhs, rhs)
                : scope->builder.CreateMul(lhs, rhs);
            break;
        case BinaryOperatorKind::Div:
            if (binding.leftClass == OperatorOperandClass::Float) {
                result = scope->builder.CreateFDiv(lhs, rhs);
            } else if (binding.leftClass == OperatorOperandClass::UnsignedInt) {
                result = scope->builder.CreateUDiv(lhs, rhs);
            } else {
                result = scope->builder.CreateSDiv(lhs, rhs);
            }
            break;
        case BinaryOperatorKind::Mod:
            result = binding.leftClass == OperatorOperandClass::UnsignedInt
                ? scope->builder.CreateURem(lhs, rhs)
                : scope->builder.CreateSRem(lhs, rhs);
            break;
        case BinaryOperatorKind::ShiftLeft:
            result = scope->builder.CreateShl(lhs, rhs);
            break;
        case BinaryOperatorKind::ShiftRight:
            result = binding.leftClass == OperatorOperandClass::UnsignedInt
                ? scope->builder.CreateLShr(lhs, rhs)
                : scope->builder.CreateAShr(lhs, rhs);
            break;
        case BinaryOperatorKind::BitAnd:
            result = scope->builder.CreateAnd(lhs, rhs);
            break;
        case BinaryOperatorKind::BitXor:
            result = scope->builder.CreateXor(lhs, rhs);
            break;
        case BinaryOperatorKind::BitOr:
            result = scope->builder.CreateOr(lhs, rhs);
            break;
        case BinaryOperatorKind::Less:
            if (binding.leftClass == OperatorOperandClass::Float) {
                result = scope->builder.CreateFCmpOLT(lhs, rhs);
            } else if (binding.leftClass == OperatorOperandClass::UnsignedInt) {
                result = scope->builder.CreateICmpULT(lhs, rhs);
            } else {
                result = scope->builder.CreateICmpSLT(lhs, rhs);
            }
            break;
        case BinaryOperatorKind::Greater:
            if (binding.leftClass == OperatorOperandClass::Float) {
                result = scope->builder.CreateFCmpOGT(lhs, rhs);
            } else if (binding.leftClass == OperatorOperandClass::UnsignedInt) {
                result = scope->builder.CreateICmpUGT(lhs, rhs);
            } else {
                result = scope->builder.CreateICmpSGT(lhs, rhs);
            }
            break;
        case BinaryOperatorKind::LessEqual:
            if (binding.leftClass == OperatorOperandClass::Float) {
                result = scope->builder.CreateFCmpOLE(lhs, rhs);
            } else if (binding.leftClass == OperatorOperandClass::UnsignedInt) {
                result = scope->builder.CreateICmpULE(lhs, rhs);
            } else {
                result = scope->builder.CreateICmpSLE(lhs, rhs);
            }
            break;
        case BinaryOperatorKind::GreaterEqual:
            if (binding.leftClass == OperatorOperandClass::Float) {
                result = scope->builder.CreateFCmpOGE(lhs, rhs);
            } else if (binding.leftClass == OperatorOperandClass::UnsignedInt) {
                result = scope->builder.CreateICmpUGE(lhs, rhs);
            } else {
                result = scope->builder.CreateICmpSGE(lhs, rhs);
            }
            break;
        case BinaryOperatorKind::Equal:
            result = binding.leftClass == OperatorOperandClass::Float
                ? scope->builder.CreateFCmpOEQ(lhs, rhs)
                : scope->builder.CreateICmpEQ(lhs, rhs);
            break;
        case BinaryOperatorKind::NotEqual:
            result = binding.leftClass == OperatorOperandClass::Float
                ? scope->builder.CreateFCmpUNE(lhs, rhs)
                : scope->builder.CreateICmpNE(lhs, rhs);
            break;
        default:
            error("unsupported binary operator binding");
        }

        return makeReadonlyValue(binding.resultType, result);
    }

    Object *emitShortCircuitBinary(HIRBinOper *bin) {
        const auto &binding = bin->getBinding();
        auto *left = compileExpr(bin->getLeft());
        auto *lhsBool = emitBoolCast(left);

        auto &context = scope->builder.getContext();
        auto *function = scope->builder.GetInsertBlock()
            ? scope->builder.GetInsertBlock()->getParent()
            : nullptr;
        if (!function) {
            error("logical short-circuit needs an active function");
        }

        auto *lhsBlock = scope->builder.GetInsertBlock();
        auto *rhsBB = llvm::BasicBlock::Create(context, "logic.rhs", function);
        auto *shortBB = llvm::BasicBlock::Create(context, "logic.short", function);
        auto *mergeBB = llvm::BasicBlock::Create(context, "logic.merge", function);

        if (binding.kind == BinaryOperatorKind::LogicalAnd) {
            scope->builder.CreateCondBr(lhsBool, rhsBB, shortBB);
        } else {
            scope->builder.CreateCondBr(lhsBool, shortBB, rhsBB);
        }

        scope->builder.SetInsertPoint(shortBB);
        scope->builder.CreateBr(mergeBB);
        auto *shortEnd = scope->builder.GetInsertBlock();

        scope->builder.SetInsertPoint(rhsBB);
        auto *right = compileExpr(bin->getRight());
        auto *rhsBool = emitBoolCast(right);
        scope->builder.CreateBr(mergeBB);
        auto *rhsEnd = scope->builder.GetInsertBlock();

        scope->builder.SetInsertPoint(mergeBB);
        auto *phi = scope->builder.CreatePHI(scope->builder.getInt1Ty(), 2);
        phi->addIncoming(
            llvm::ConstantInt::getFalse(scope->builder.getInt1Ty()),
            binding.kind == BinaryOperatorKind::LogicalAnd ? shortEnd : rhsEnd);
        phi->addIncoming(
            llvm::ConstantInt::getTrue(scope->builder.getInt1Ty()),
            binding.kind == BinaryOperatorKind::LogicalOr ? shortEnd : rhsEnd);
        if (binding.kind == BinaryOperatorKind::LogicalAnd) {
            phi->setIncomingValue(1, rhsBool);
        } else {
            phi->setIncomingValue(0, rhsBool);
        }
        (void)lhsBlock;
        return makeReadonlyValue(boolTy, phi);
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
            } else if (abiSignature.resultInfo.packedRegisterAggregate) {
                auto *retSlot = scope->retVal();
                llvm::Value *retValue = nullptr;
                if (retSlot->isVariable() && !retSlot->isRegVal() &&
                    retSlot->getllvmValue()) {
                    retValue = loadNativeAbiDirectValue(
                        scope->builder, *typeMgr, retSlot->getType(),
                        retSlot->getllvmValue());
                } else {
                    retValue = packNativeAbiDirectValue(
                        scope->builder, *typeMgr, retSlot->getType(),
                        retSlot->get(scope));
                }
                scope->builder.CreateRet(retValue);
            } else {
                scope->builder.CreateRet(scope->retVal()->get(scope));
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
            functionError(hirFunc, "HIR function missing LLVM function");
        }
        if (!llvmFunc->empty()) {
            return;
        }
        if (!hirFunc->getBody()) {
            return;
        }

        auto *entry = llvm::BasicBlock::Create(context, "entry", llvmFunc);
        global->builder.SetInsertPoint(entry);
        scope = new FuncScope(global);

        auto *funcType = hirFunc->getFuncType();
        if (!funcType) {
            functionError(hirFunc, "invalid function type");
        }
        abiSignature =
            classifyFunctionAbi(*typeMgr, funcType, hirFunc->hasSelfBinding());
        returnByPointer = abiSignature.hasIndirectResult;

        if (hirFunc->hasSelfBinding()) {
            scope->structTy =
                asUnqualified<StructType>(hirFunc->getSelfBinding().object->getType());
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
            functionError(hirFunc, "not all paths return a value");
        }

        size_t llvmArgIndex = 0;
        if (hirFunc->hasSelfBinding()) {
            auto &binding = hirFunc->getSelfBinding();
            auto argIt = llvmFunc->arg_begin();
            std::advance(argIt, llvmArgIndex);
            auto *incomingSelf = binding.object->getType()->newObj(Object::VARIABLE);
            incomingSelf->setllvmValue(&*argIt);
            auto *selfObj = materializeBinding(binding.object, incomingSelf);
            scope->addObj(llvm::StringRef(binding.name), selfObj);
            emitDebugDeclare(debug, scope, debugSubprogram, selfObj, binding.name,
                             selfObj->getType(), binding.loc, 1);
            ++llvmArgIndex;
        }

        if (retType) {
            Object *retSlot = nullptr;
            if (returnByPointer) {
                if (llvmFunc->arg_size() < 1) {
                    functionError(hirFunc, "function is missing hidden return slot argument");
                }
                auto argIt = llvmFunc->arg_begin();
                std::advance(argIt, llvmArgIndex);
                retSlot = retType->newObj(Object::VARIABLE);
                retSlot->setllvmValue(&*argIt);
                ++llvmArgIndex;
            } else {
                retSlot = materializeLocal(retType, nullptr);
            }
            scope->initRetVal(retSlot);
            if (hirFunc->isTopLevelEntry() && retType == i32Ty) {
                retSlot->set(scope, new ConstVar(i32Ty, int32_t(0)));
            }
            auto *retBB = llvm::BasicBlock::Create(context, "return", llvmFunc);
            scope->initRetBlock(retBB);
        }

        unsigned debugArgIndex = hirFunc->hasSelfBinding() ? 2 : 1;

        auto expectedArgs = abiSignature.llvmType
            ? abiSignature.llvmType->getNumParams()
            : 0;
        if (llvmFunc->arg_size() != expectedArgs) {
            functionError(hirFunc, "function argument number mismatch");
        }
        const std::size_t sourceParamOffset = hirFunc->hasSelfBinding() ? 1 : 0;
        std::size_t sourceParamIndex = sourceParamOffset;
        for (const auto &binding : hirFunc->getParams()) {
            auto argIt = llvmFunc->arg_begin();
            std::advance(argIt, llvmArgIndex);
            Object *argObj = nullptr;
            const auto &argInfo = abiSignature.argInfo(sourceParamIndex);
            auto passKind = argInfo.passKind;
            if (passKind == AbiPassKind::IndirectRef) {
                auto *incomingArg = binding.object->getType()->newObj(Object::VARIABLE);
                incomingArg->setllvmValue(&*argIt);
                argObj = materializeBinding(binding.object, incomingArg);
            } else if (passKind == AbiPassKind::IndirectValue) {
                argObj = materializeIndirectValueBinding(binding.object, &*argIt);
            } else {
                argObj = materializeDirectValueBinding(binding.object, &*argIt,
                                                       argInfo.packedRegisterAggregate);
            }
            scope->addObj(llvm::StringRef(binding.name), argObj);
            emitDebugDeclare(debug, scope, debugSubprogram, argObj, binding.name,
                             argObj->getType(), binding.loc, debugArgIndex);
            ++llvmArgIndex;
            ++debugArgIndex;
            ++sourceParamIndex;
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

    auto resolvedModule = resolveModule(globalScope, root, nullptr);
    auto hirModule = analyzeModule(globalScope, *resolvedModule, nullptr);
    emitHIRModule(global, hirModule.get(), emitDebugInfo,
                  globalScope->module.getName().str());
}

void
emitHIRModule(Scope *global, HIRModule *module, bool emitDebugInfo,
              const std::string &primarySourcePath) {
    auto *globalScope = dynamic_cast<GlobalScope *>(global);
    assert(globalScope);
    initBuildinType(globalScope);

    std::unique_ptr<DebugInfoContext> debug;
    if (emitDebugInfo) {
        debug = std::make_unique<DebugInfoContext>(
            globalScope->module, *globalScope->types(),
            primarySourcePath.empty() ? globalScope->module.getName().str()
                                      : primarySourcePath);
    }
    ModuleCompiler(globalScope, module, debug.get());

    if (debug) {
        debug->finalize();
    }
}

}  // namespace lona
