#include "../abi/abi.hh"
#include "../abi/native_abi.hh"
#include "../type/buildin.hh"
#include "../type/scope.hh"
#include "../visitor.hh"
#include "lona/ast/array_dim.hh"
#include "lona/ast/astnode.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/err/err.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/call_target_tools.hh"
#include "lona/sema/collect_internal.hh"
#include "lona/sema/hir.hh"
#include "lona/sema/initializer_semantics.hh"
#include "lona/sema/operator_resolver.hh"
#include "lona/sym/func.hh"
#include "parser.hh"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <llvm-18/llvm/BinaryFormat/Dwarf.h>
#include <llvm-18/llvm/IR/BasicBlock.h>
#include <llvm-18/llvm/IR/ConstantFold.h>
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
namespace collect_decl_impl {

const char *
topLevelDeclKindName(TopLevelDeclKind kind) {
    switch (kind) {
        case TopLevelDeclKind::StructType:
            return "struct";
        case TopLevelDeclKind::Function:
            return "top-level function";
        case TopLevelDeclKind::Global:
            return "global";
    }
    return "top-level declaration";
}

std::string
topLevelDeclConflictHint(TopLevelDeclKind incoming, TopLevelDeclKind existing,
                         const std::string &name) {
    if ((incoming == TopLevelDeclKind::StructType &&
         existing == TopLevelDeclKind::Function) ||
        (incoming == TopLevelDeclKind::Function &&
         existing == TopLevelDeclKind::StructType)) {
        return "Type names reserve constructor syntax like `" + name +
               "(...)`. Rename the function, for example `make" + name +
               "`, or choose a different type name.";
    }
    return "Choose distinct names for top-level declarations in the same "
           "module.";
}

void
recordTopLevelDeclName(
    std::unordered_map<std::string, std::pair<TopLevelDeclKind, location>>
        &seen,
    const std::string &name, TopLevelDeclKind kind, const location &loc) {
    auto found = seen.find(name);
    if (found != seen.end()) {
        if (found->second.first != kind) {
            error(loc,
                  std::string(topLevelDeclKindName(kind)) + " `" + name +
                      "` conflicts with " +
                      topLevelDeclKindName(found->second.first) + " `" + name +
                      "`",
                  topLevelDeclConflictHint(kind, found->second.first, name));
        }
        return;
    }
    seen.emplace(name, std::make_pair(kind, loc));
}

std::vector<string>
extractParamNames(AstFuncDecl *node) {
    std::vector<string> names;
    if (!node || !node->args) {
        return names;
    }
    names.reserve(node->args->size());
    for (auto *arg : *node->args) {
        auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
        if (!varDecl) {
            continue;
        }
        names.push_back(varDecl->field);
    }
    return names;
}

std::vector<BindingKind>
extractParamBindingKinds(AstFuncDecl *node, bool withImplicitSelf) {
    std::vector<BindingKind> kinds;
    if (withImplicitSelf) {
        kinds.push_back(BindingKind::Value);
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

TypeClass *
methodReceiverPointeeType(TypeTable *typeMgr, StructType *methodParent,
                          AccessKind receiverAccess) {
    if (!typeMgr || !methodParent) {
        return nullptr;
    }
    if (receiverAccess == AccessKind::GetSet) {
        return methodParent;
    }
    return typeMgr->createConstType(methodParent);
}

TypeClass *
interfaceMethodReceiverPointeeType(ModuleInterface *interface,
                                   StructType *methodParent,
                                   AccessKind receiverAccess) {
    if (!interface || !methodParent) {
        return nullptr;
    }
    if (receiverAccess == AccessKind::GetSet) {
        return methodParent;
    }
    return interface->getOrCreateConstType(methodParent);
}

void
validateFunctionReceiverAccess(AstFuncDecl *node, StructType *methodParent) {
    if (!node || methodParent || node->receiverAccess == AccessKind::GetOnly) {
        return;
    }
    error(node->loc, "`set def` is only valid on struct methods",
          "Move this declaration into a struct, or remove the `set` receiver "
          "modifier.");
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
        return describeTypeNode(node,
                                type ? toStdString(type->full_name) : "void");
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
    return type && (type->isOpaque() || type->isReprC());
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
    if (!structType || !structType->isOpaque()) {
        return;
    }
    auto typeName = describeExternCType(type, typeNode);
    error(loc,
          "opaque struct `" + typeName + "` cannot be used by value in " +
              context,
          "Use `" + typeName +
              "*` instead. Opaque structs are only supported behind pointers.");
}

void
validateStructDeclShape(AstStructDecl *node) {
    if (!node) {
        return;
    }
    if (node->isOpaqueDecl() && node->body) {
        error(node->loc,
              "opaque struct `" + toStdString(node->name) +
                  "` cannot declare fields or methods",
              "Use `struct " + toStdString(node->name) +
                  "` for an opaque declaration, or add a body without the "
                  "opaque form.");
    }
}

std::string
describeStructFieldSyntax(AstVarDecl *fieldDecl);
void
validateEmbeddedStructField(AstStructDecl *structDecl, AstVarDecl *fieldDecl,
                            TypeClass *fieldType);
void
insertStructMember(AstStructDecl *structDecl, AstVarDecl *fieldDecl,
                   TypeClass *fieldType,
                   llvm::StringMap<StructType::ValueTy> &members,
                   llvm::StringMap<AccessKind> &memberAccess,
                   llvm::StringSet<> &embeddedMembers,
                   std::unordered_map<std::string, location> &seenMembers,
                   int &nextMemberIndex);

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
              "#[extern \"C\"] function `" + funcName + "` uses unsupported " +
                  subject + ": " + typeName,
              "Callback support is not implemented in C FFI v0 yet.");
    }
    if (auto *pointerType = asUnqualified<PointerType>(type)) {
        if (!isCCompatiblePointerTarget(pointerType->getPointeeType())) {
            error(loc,
                  "#[extern \"C\"] function `" + funcName +
                      "` uses unsupported " + subject + ": " + typeName,
                  "Use pointers to scalars, pointers, opaque `struct` "
                  "declarations, or `#[repr \"C\"] struct` types. Ordinary "
                  "Lona structs cannot cross the C FFI boundary.");
        }
        return;
    }
    if (auto *indexableType = asUnqualified<IndexablePointerType>(type)) {
        if (!isCCompatiblePointerTarget(indexableType->getElementType())) {
            error(loc,
                  "#[extern \"C\"] function `" + funcName +
                      "` uses unsupported " + subject + ": " + typeName,
                  "Use pointers to scalars, pointers, opaque `struct` "
                  "declarations, or `#[repr \"C\"] struct` types. Ordinary "
                  "Lona structs cannot cross the C FFI boundary.");
        }
        return;
    }
    if (asUnqualified<TupleType>(type)) {
        error(loc,
              "#[extern \"C\"] function `" + funcName + "` uses unsupported " +
                  subject + ": " + typeName,
              "Flatten the tuple into scalar parameters or pass a pointer "
              "instead.");
    }
    if (isExternCByValueAggregateType(type)) {
        error(loc,
              "#[extern \"C\"] function `" + funcName + "` uses unsupported " +
                  subject + ": " + typeName,
              "Pass a pointer instead. C FFI v0 does not support aggregate "
              "values at the boundary yet.");
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
              "struct field `" + describeStructFieldSyntax(fieldDecl) +
                  "` cannot use a const-qualified storage type",
              "Struct fields must keep full read/write access during "
              "initialization. Use pointer views like `T const*` or `T "
              "const[*]`, and reserve field immutability for a future "
              "`readonly` feature.");
    }
    rejectOpaqueStructByValue(
        fieldType, fieldDecl->typeNode, fieldDecl->loc,
        "struct field `" + describeStructFieldSyntax(fieldDecl) + "`");
    if (!structDecl->isReprC()) {
        return;
    }
    if (isCCompatibleReprCFieldType(fieldType)) {
        return;
    }

    auto fieldTypeName = describeExternCType(fieldType, fieldDecl->typeNode);
    error(
        fieldDecl->loc,
        "#[repr \"C\"] struct `" + toStdString(structDecl->name) + "` field `" +
            describeStructFieldSyntax(fieldDecl) +
            "` uses unsupported type: " + fieldTypeName,
        "Use only C-compatible field types: scalars, raw pointers, fixed "
        "arrays of C-compatible elements, or nested `#[repr \"C\"]` structs.");
}

std::string
embeddedFieldAccessName(TypeClass *fieldType) {
    auto *structType = asUnqualified<StructType>(fieldType);
    if (!structType) {
        return {};
    }
    auto fullName = toStdString(structType->full_name);
    auto separator = fullName.rfind('.');
    return separator == std::string::npos ? fullName
                                          : fullName.substr(separator + 1);
}

std::string
effectiveStructFieldName(AstVarDecl *fieldDecl, TypeClass *fieldType) {
    if (!fieldDecl) {
        return {};
    }
    if (!fieldDecl->isEmbeddedField()) {
        return toStdString(fieldDecl->field);
    }
    return embeddedFieldAccessName(fieldType);
}

std::string
describeStructFieldSyntax(AstVarDecl *fieldDecl) {
    if (!fieldDecl) {
        return "<unknown field>";
    }
    if (!fieldDecl->isEmbeddedField()) {
        return toStdString(fieldDecl->field);
    }
    return "_ " + describeTypeNode(fieldDecl->typeNode, "void");
}

void
validateEmbeddedStructField(AstStructDecl *structDecl, AstVarDecl *fieldDecl,
                            TypeClass *fieldType) {
    if (!fieldDecl || !fieldDecl->isEmbeddedField()) {
        return;
    }
    if (fieldDecl->bindingKind == BindingKind::Ref) {
        error(fieldDecl->loc,
              "embedded field `" + describeStructFieldSyntax(fieldDecl) +
                  "` cannot use `ref` binding",
              "Embed the struct value directly, or store an explicit pointer "
              "field instead.");
    }
    if (fieldDecl->accessKind == AccessKind::GetSet) {
        error(fieldDecl->loc,
              "embedded field `" + describeStructFieldSyntax(fieldDecl) +
                  "` cannot use `set`",
              "V1 only supports `_ T` for embedded fields. If you need a "
              "writable named field, declare it explicitly.");
    }
    if (!asUnqualified<StructType>(fieldType)) {
        error(fieldDecl->loc,
              "embedded field `" + describeStructFieldSyntax(fieldDecl) +
                  "` must use a struct type",
              "Write `_ SomeStruct` or `_ dep.SomeStruct`. Non-struct "
              "embedding is not supported.");
    }
    auto accessName = effectiveStructFieldName(fieldDecl, fieldType);
    if (accessName.empty()) {
        error(fieldDecl->loc,
              "embedded field `" + describeStructFieldSyntax(fieldDecl) +
                  "` is missing a usable access name",
              "Use a named struct type like `_ Inner` or `_ dep.Inner`.");
    }
}

void
insertStructMember(AstStructDecl *structDecl, AstVarDecl *fieldDecl,
                   TypeClass *fieldType,
                   llvm::StringMap<StructType::ValueTy> &members,
                   llvm::StringMap<AccessKind> &memberAccess,
                   llvm::StringSet<> &embeddedMembers,
                   std::unordered_map<std::string, location> &seenMembers,
                   int &nextMemberIndex) {
    auto memberName = effectiveStructFieldName(fieldDecl, fieldType);
    if (memberName.empty()) {
        internalError(fieldDecl ? fieldDecl->loc : location(),
                      "struct field is missing its effective member name",
                      "This looks like a struct-member collection bug.");
    }

    auto [seenIt, inserted] = seenMembers.emplace(memberName, fieldDecl->loc);
    if (!inserted) {
        auto fieldRole = fieldDecl && fieldDecl->isEmbeddedField()
                             ? "embedded field access name"
                             : "field";
        auto structName = structDecl ? toStdString(structDecl->name)
                                     : std::string("<unknown>");
        error(fieldDecl->loc,
              "struct `" + structName + "` " + fieldRole + " `" + memberName +
                  "` conflicts with an existing member",
              "Rename the field, or use an explicit named field instead of "
              "embedding here.");
    }

    auto memberKey = llvm::StringRef(memberName);
    members.insert(
        {memberKey, StructType::ValueTy{fieldType, nextMemberIndex++}});
    memberAccess[memberKey] = fieldDecl->accessKind;
    if (fieldDecl->isEmbeddedField()) {
        embeddedMembers.insert(memberKey);
    }
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
              "#[extern \"C\"] method `" + funcName + "` is not supported",
              "Declare a top-level wrapper function instead. C FFI v0 only "
              "supports top-level functions.");
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
                      "#[extern \"C\"] function `" + funcName +
                          "` parameter `" + toStdString(varDecl->field) +
                          "` cannot use `ref` binding",
                      "Use an explicit pointer type like `i32*` instead.");
            }
            auto *argType = argTypeIndex < argTypes.size()
                                ? argTypes[argTypeIndex]
                                : nullptr;
            rejectOpaqueStructByValue(argType, varDecl->typeNode, varDecl->loc,
                                      "parameter `" +
                                          toStdString(varDecl->field) +
                                          "` in function `" + funcName + "`");
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

std::string
resolveTopLevelName(const CompilationUnit *unit, const string &name,
                    bool exportNamespace) {
    auto resolved = toStdString(name);
    if (!unit || !exportNamespace) {
        return resolved;
    }
    return toStdString(unit->moduleName() + "." + name);
}

std::string
resolveFunctionSymbolName(const CompilationUnit *unit, const string &name,
                          AbiKind abiKind, bool exportNamespace) {
    if (abiKind == AbiKind::C) {
        return toStdString(name);
    }
    return resolveTopLevelName(unit, name, exportNamespace);
}

std::string
resolveGlobalSymbolName(const CompilationUnit *unit, const string &name,
                        bool isExtern, bool exportNamespace) {
    if (isExtern) {
        return toStdString(name);
    }
    return resolveTopLevelName(unit, name, exportNamespace);
}

TypeClass *
resolveTypeNode(TypeTable *typeMgr, const CompilationUnit *unit,
                TypeNode *node) {
    if (!typeMgr) {
        return nullptr;
    }
    if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
        auto rawStorage = baseTypeName(base);
        auto rawName = llvm::StringRef(rawStorage);
        if (isReservedInitialListTypeName(rawName)) {
            errorReservedInitialListType(base->loc);
        }
    }
    return unit ? unit->resolveType(typeMgr, node) : typeMgr->getType(node);
}

void
rejectBareFunctionType(TypeClass *type, TypeNode *node,
                       const std::string &context, const location &loc) {
    if (!type || !type->as<FuncType>()) {
        return;
    }
    error(loc, context + ": " + describeTypeNode(node, "void"),
          "Use an explicit function pointer type instead of a bare function "
          "type.");
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
    auto moduleName = toStringRef(unit.moduleName());
    auto *existing = scope.getObj(moduleName);
    if (existing) {
        auto *moduleObject = existing->as<ModuleObject>();
        if (!moduleObject || moduleObject->unit() != &unit) {
            error("module namespace `" + toStdString(unit.moduleName()) +
                  "` conflicts with an existing global symbol");
        }
        return;
    }
    scope.addObj(moduleName, new ModuleObject(&unit));
}

StructType *
declareStructType(TypeTable *typeMgr, AstStructDecl *node,
                  CompilationUnit *unit, bool exportNamespace) {
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
                StructType *methodParent, CompilationUnit *unit,
                bool exportNamespace) {
    validateFunctionReceiverAccess(node, methodParent);
    auto &func_name = node->name;
    auto resolvedFunctionName = resolveFunctionSymbolName(
        unit, func_name, node ? node->abiKind : AbiKind::Native,
        exportNamespace);
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
                          "` conflicts with struct `" + toStdString(func_name) +
                          "`",
                      "Type names reserve constructor syntax like `" +
                          toStdString(func_name) +
                          "(...)`. Rename the function, for example `make" +
                          toStdString(func_name) + "`.");
            }
            unit->bindLocalFunction(toStdString(func_name),
                                    resolvedFunctionName);
        }
        if (auto *existing =
                scope.getObj(llvm::StringRef(resolvedFunctionName))) {
            auto *func = existing->as<Function>();
            if (!func) {
                error(node->loc, "top-level function `" +
                                     toStdString(func_name) +
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
            error(node->loc, "unknown return type for function `" +
                                 toStdString(func_name) + "`: " +
                                 describeTypeNode(node->retType, "void"));
        }
        rejectBareFunctionType(loretType, node->retType,
                               "unsupported bare function return type for `" +
                                   toStdString(func_name) + "`",
                               node->loc);
        rejectOpaqueStructByValue(
            loretType, node->retType, node->loc,
            "return type of function `" + toStdString(func_name) + "`");
    }

    if (methodParent) {
        loargs.push_back(typeMgr->createPointerType(methodReceiverPointeeType(
            typeMgr, methodParent, node->receiverAccess)));
    }

    if (node->args) {
        for (auto *arg : *node->args) {
            if (!arg->is<AstVarDecl>()) {
                error(node->loc, "invalid function parameter declaration in `" +
                                     toStdString(func_name) + "`");
            }
            auto *varDecl = arg->as<AstVarDecl>();
            auto *type = resolveTypeNode(typeMgr, unit, varDecl->typeNode);
            if (!type) {
                error(varDecl->loc,
                      "unknown type for function parameter `" +
                          toStdString(varDecl->field) + "` in `" +
                          toStdString(func_name) +
                          "`: " + describeTypeNode(varDecl->typeNode, "void"));
            }
            rejectBareFunctionType(
                type, varDecl->typeNode,
                "unsupported bare function parameter type for `" +
                    toStdString(varDecl->field) + "` in `" +
                    toStdString(func_name) + "`",
                varDecl->loc);
            rejectOpaqueStructByValue(
                type, varDecl->typeNode, varDecl->loc,
                "parameter `" + toStdString(varDecl->field) +
                    "` in function `" + toStdString(func_name) + "`");
            loargs.push_back(type);
        }
    }

    validateExternCFunctionSignature(node, methodParent, loargs, loretType);
    auto *lofuncType = typeMgr->getOrCreateFunctionType(
        loargs, loretType, std::move(loargKinds), node->abiKind);
    if (methodParent && !methodParent->getMethodType(llvm::StringRef(
                            func_name.tochara(), func_name.size()))) {
        methodParent->addMethodType(
            llvm::StringRef(func_name.tochara(), func_name.size()), lofuncType,
            extractParamNames(node));
    }

    std::string llvmName = resolvedFunctionName.empty() ? toStdString(func_name)
                                                        : resolvedFunctionName;
    if (methodParent) {
        llvmName = toStdString(methodParent->full_name) + "." + llvmName;
    }

    auto *llfunc = llvm::Function::Create(
        getFunctionAbiLLVMType(*typeMgr, lofuncType, methodParent != nullptr),
        llvm::Function::ExternalLinkage, llvm::Twine(llvmName),
        typeMgr->getModule());
    annotateFunctionAbi(*llfunc, lofuncType->getAbiKind());
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

}  // namespace collect_decl_impl

}  // namespace lona
