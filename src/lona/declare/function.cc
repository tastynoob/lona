#include "lona/declare/support.hh"

#include "lona/abi/abi.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/err/err.hh"
#include "lona/sema/initializer.hh"
#include "lona/sema/moduleentry.hh"
#include "lona/type/buildin.hh"

namespace lona {
namespace declarationsupport_impl {

namespace {

[[noreturn]] void
reportLocalFunctionConflict(AstFuncDecl *node, llvm::StringRef functionName,
                            FuncType *existingType, FuncType *incomingType) {
    std::string message =
        "conflicting declarations for function `" + functionName.str() + "`";
    if (existingType && incomingType) {
        message += ": `" + describeResolvedType(existingType) + "` vs `" +
                   describeResolvedType(incomingType) + "`";
    }
    error(node ? node->loc : location(), std::move(message),
          "Make duplicated `#[extern \"C\"]` function declarations use the "
          "same signature in every module.");
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
buildAppliedStructTypeName(llvm::StringRef templateName,
                           const std::vector<TypeClass *> &typeArgs) {
    std::string name = templateName.str() + "[";
    for (std::size_t i = 0; i < typeArgs.size(); ++i) {
        if (i != 0) {
            name += ", ";
        }
        auto *typeArg = typeArgs[i];
        name += typeArg ? toStdString(typeArg->full_name) : "<null>";
    }
    name += "]";
    return name;
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
isExternCTraitObjectType(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    return storageType && storageType->as<DynTraitType>();
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
    if (isExternCTraitObjectType(type)) {
        error(loc,
              "#[extern \"C\"] function `" + funcName + "` uses unsupported " +
                  subject + ": " + typeName,
              "Trait objects are internal runtime values in trait v0. Pass "
              "an explicit opaque pointer type across the C boundary "
              "instead.");
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

std::string
resolveTopLevelName(const CompilationUnit *unit, const string &name,
                    bool exportNamespace) {
    auto resolved = toStdString(name);
    if (!unit || !exportNamespace) {
        return resolved;
    }
    return toStdString(unit->exportNamespacePrefix() + "." + name);
}

std::string
resolveFunctionSymbolName(const CompilationUnit *unit, const string &name,
                          AbiKind abiKind, bool exportNamespace,
                          Scope *scope = nullptr) {
    if (abiKind == AbiKind::C) {
        return toStdString(name);
    }
    auto resolved = resolveTopLevelName(unit, name, exportNamespace);
    if (scope && unit && !exportNamespace &&
        scope->getObj(llvm::StringRef(resolved))) {
        return toStdString(unit->exportNamespacePrefix() + "." + name);
    }
    return resolved;
}

bool
isBuiltinScalarExtensionBase(TypeClass *type) {
    auto *storageType = stripTopLevelConst(type);
    return storageType &&
           (storageType == u8Ty || storageType == i8Ty ||
            storageType == u16Ty || storageType == i16Ty ||
            storageType == u32Ty || storageType == i32Ty ||
            storageType == u64Ty || storageType == i64Ty ||
            storageType == usizeTy || storageType == f32Ty ||
            storageType == f64Ty || storageType == boolTy);
}

[[noreturn]] void
errorInvalidExtensionReceiver(AstFuncDecl *node, const std::string &message,
                              const std::string &hint) {
    error(node ? node->loc : location(), message, hint);
}

}  // namespace

std::string
resolveStructMethodOwnerTypeName(StructType *methodParent) {
    if (!methodParent) {
        return {};
    }
    if (methodParent->isAppliedTemplateInstance() &&
        !methodParent->getAppliedTemplateName().empty()) {
        return buildAppliedStructTypeName(
            llvm::StringRef(methodParent->getAppliedTemplateName().tochara(),
                            methodParent->getAppliedTemplateName().size()),
            methodParent->getAppliedTypeArgs());
    }
    return toStdString(methodParent->full_name);
}

std::string
resolveStructMethodSymbolName(StructType *methodParent,
                              llvm::StringRef methodName) {
    if (!methodParent) {
        return methodName.str();
    }
    auto ownerTypeName = resolveStructMethodOwnerTypeName(methodParent);
    if (methodParent->isAppliedTemplateInstance()) {
        return mangleModuleEntryComponent(string(ownerTypeName)) + "." +
               methodName.str();
    }
    return ownerTypeName + "." + methodName.str();
}

std::string
resolveTraitMethodSymbolName(StructType *methodParent, llvm::StringRef traitName,
                             llvm::StringRef methodName) {
    if (!methodParent) {
        return methodName.str();
    }
    return resolveStructMethodOwnerTypeName(methodParent) + ".__trait__." +
           mangleModuleEntryComponent(traitName) + "." + methodName.str();
}

std::vector<string>
extractParamNames(AstFuncDecl *node, std::size_t skipLeadingArgs) {
    std::vector<string> names;
    if (!node || !node->args) {
        return names;
    }
    if (skipLeadingArgs >= node->args->size()) {
        return names;
    }
    names.reserve(node->args->size() - skipLeadingArgs);
    std::size_t index = 0;
    for (auto *arg : *node->args) {
        if (index++ < skipLeadingArgs) {
            continue;
        }
        auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
        if (!varDecl) {
            continue;
        }
        names.push_back(varDecl->field);
    }
    return names;
}

std::vector<BindingKind>
extractParamBindingKinds(AstFuncDecl *node, std::size_t skipLeadingArgs,
                         bool prependImplicitSelf) {
    std::vector<BindingKind> kinds;
    if (prependImplicitSelf) {
        kinds.push_back(BindingKind::Value);
    }
    if (!node || !node->args) {
        return kinds;
    }
    if (skipLeadingArgs >= node->args->size()) {
        return kinds;
    }
    kinds.reserve(kinds.size() + node->args->size() - skipLeadingArgs);
    std::size_t index = 0;
    for (auto *arg : *node->args) {
        if (index++ < skipLeadingArgs) {
            continue;
        }
        auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
        kinds.push_back(varDecl ? varDecl->bindingKind : BindingKind::Value);
    }
    return kinds;
}

ExtensionReceiverInfo
classifyExtensionReceiver(TypeTable *typeMgr, const CompilationUnit *unit,
                          AstFuncDecl *node) {
    ExtensionReceiverInfo info;
    if (!node || !node->hasExtensionReceiver()) {
        return info;
    }

    auto *receiverTypeNode = node->extensionReceiverType();
    auto *receiverType = resolveTypeNode(typeMgr, unit, receiverTypeNode);
    if (!receiverType) {
        error(node->loc,
              "unknown extension receiver type for `" +
                  toStdString(node->name) + "`: " +
                  describeTypeNode(receiverTypeNode, "void"),
              "Declare the receiver type before defining this extension "
              "method.");
    }

    if (auto *pointerNode = dynamic_cast<PointerTypeNode *>(receiverTypeNode)) {
        if (pointerNode->dim != 1) {
            errorInvalidExtensionReceiver(
                node,
                "extension receiver `" +
                    describeTypeNode(receiverTypeNode, "void") +
                    "` is not supported",
                "Borrowed extension receivers must use a single pointer like "
                "`(T const*)` or `(T*)`.");
        }
        auto *baseNode = pointerNode->base;
        if (dynamic_cast<PointerTypeNode *>(baseNode) ||
            dynamic_cast<IndexablePointerTypeNode *>(baseNode) ||
            dynamic_cast<ArrayTypeNode *>(baseNode) ||
            dynamic_cast<TupleTypeNode *>(baseNode) ||
            dynamic_cast<FuncPtrTypeNode *>(baseNode) ||
            dynamic_cast<DynTypeNode *>(baseNode)) {
            errorInvalidExtensionReceiver(
                node,
                "extension receiver `" +
                    describeTypeNode(receiverTypeNode, "void") +
                    "` is not supported",
                "Borrowed extension receivers only support builtin scalar or "
                "concrete struct base types in v0.");
        }
        auto *baseType = resolveTypeNode(typeMgr, unit, baseNode);
        if (!baseType) {
            error(node->loc,
                  "unknown extension receiver base type for `" +
                      toStdString(node->name) + "`: " +
                      describeTypeNode(baseNode, "void"));
        }
        if (!isBuiltinScalarExtensionBase(baseType) &&
            !stripTopLevelConst(baseType)->as<StructType>()) {
            errorInvalidExtensionReceiver(
                node,
                "extension receiver `" +
                    describeTypeNode(receiverTypeNode, "void") +
                    "` is not supported",
                "Borrowed extension receivers only support builtin scalar or "
                "concrete struct base types in v0.");
        }
        auto *pointerType = asUnqualified<PointerType>(receiverType);
        if (!pointerType) {
            internalError(node->loc,
                          "extension receiver pointer type did not resolve to "
                          "a pointer",
                          "This looks like an extension receiver type "
                          "resolution bug.");
        }
        info.kind = isConstQualifiedType(pointerType->getPointeeType())
                        ? ExtensionReceiverKind::BorrowedReadOnly
                        : ExtensionReceiverKind::BorrowedReadWrite;
        info.receiverType = receiverType;
        info.baseType = stripTopLevelConst(baseType);
        info.receiverTypeSpelling = toStdString(receiverType->full_name);
        info.baseTypeSpelling = info.baseType
                                    ? toStdString(info.baseType->full_name)
                                    : std::string();
        return info;
    }

    if (dynamic_cast<IndexablePointerTypeNode *>(receiverTypeNode) ||
        dynamic_cast<ArrayTypeNode *>(receiverTypeNode) ||
        dynamic_cast<TupleTypeNode *>(receiverTypeNode) ||
        dynamic_cast<FuncPtrTypeNode *>(receiverTypeNode) ||
        dynamic_cast<DynTypeNode *>(receiverTypeNode) ||
        stripTopLevelConst(receiverType)->as<StructType>()) {
        errorInvalidExtensionReceiver(
            node,
            "extension receiver `" +
                describeTypeNode(receiverTypeNode, "void") +
                "` is not supported",
            "Value receivers only support builtin scalar types in v0; "
            "composite types must use `(T const*)` or `(T*)` receivers.");
    }
    if (!isBuiltinScalarExtensionBase(receiverType)) {
        errorInvalidExtensionReceiver(
            node,
            "extension receiver `" +
                describeTypeNode(receiverTypeNode, "void") +
                "` is not supported",
            "Value receivers only support builtin scalar types in v0.");
    }
    info.kind = ExtensionReceiverKind::Value;
    info.receiverType = receiverType;
    info.baseType = receiverType;
    info.receiverTypeSpelling = toStdString(receiverType->full_name);
    info.baseTypeSpelling = toStdString(receiverType->full_name);
    return info;
}

std::string
resolveExtensionMethodSymbolName(const CompilationUnit *unit,
                                 const std::string &receiverTypeSpelling,
                                 llvm::StringRef methodName,
                                 bool exportNamespace) {
    (void)exportNamespace;
    auto receiverKey =
        mangleModuleEntryComponent(string(receiverTypeSpelling));
    auto prefix = unit ? toStdString(unit->exportNamespacePrefix())
                       : std::string();
    if (prefix.empty()) {
        return receiverKey + "." + methodName.str();
    }
    return prefix + "." + receiverKey + "." + methodName.str();
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
    if (!node || node->receiverAccess == AccessKind::GetOnly) {
        return;
    }
    if (node->hasExtensionReceiver()) {
        error(node->loc, "`set def` is not valid on extension methods",
              "Use `def (T*).name(...)` for writable borrowed receivers.");
    }
    if (methodParent) {
        return;
    }
    error(node->loc, "`set def` is only valid on struct methods",
          "Move this declaration into a struct, or remove the `set` receiver "
          "modifier.");
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

Function *
declareFunction(Scope &scope, TypeTable *typeMgr, AstFuncDecl *node,
                StructType *methodParent, CompilationUnit *unit,
                bool exportNamespace) {
    validateFunctionReceiverAccess(node, methodParent);
    if (node && node->hasExtensionReceiver()) {
        if (methodParent) {
            error(node->loc,
                  "extension methods must be declared at top level",
                  "Move `def " + describeTypeNode(node->extensionReceiverType(),
                                                   "void") +
                      "." + toStdString(node->name) +
                      "(...)` out of the struct or impl body.");
        }
        return declareExtensionFunction(scope, typeMgr, node, unit,
                                        exportNamespace);
    }
    if (node && node->hasTypeParams() && methodParent) {
        return nullptr;
    }
    auto &funcName = node->name;
    auto resolvedFunctionName = resolveFunctionSymbolName(
        unit, funcName, node ? node->abiKind : AbiKind::Native,
        exportNamespace, &scope);
    Function *existingFunction = nullptr;
    if (methodParent) {
        if (auto *existing = typeMgr->getMethodFunction(
                methodParent,
                llvm::StringRef(funcName.tochara(), funcName.size()))) {
            return existing;
        }
    } else {
        if (unit) {
            if (unit->importsModule(toStdString(funcName))) {
                error(node->loc,
                      "top-level function `" + toStdString(funcName) +
                          "` conflicts with imported module alias `" +
                          toStdString(funcName) + "`",
                      "Rename the function so `" + toStdString(funcName) +
                          ".xxx` continues to refer to the imported module.");
            }
            if (unit->findLocalType(toStdString(funcName)) != nullptr) {
                error(node->loc,
                      "top-level function `" + toStdString(funcName) +
                          "` conflicts with struct `" + toStdString(funcName) +
                          "`",
                      "Type names reserve constructor syntax like `" +
                          toStdString(funcName) +
                          "(...)`. Rename the function, for example `make" +
                          toStdString(funcName) + "`.");
            }
            unit->bindLocalFunction(toStdString(funcName),
                                    resolvedFunctionName);
        }
        if (auto *existing =
                scope.getObj(llvm::StringRef(resolvedFunctionName))) {
            existingFunction = existing->as<Function>();
            if (!existingFunction) {
                error(node->loc, "top-level function `" +
                                     toStdString(funcName) +
                                     "` conflicts with module namespace `" +
                                     resolvedFunctionName + "`");
            }
        }
    }

    std::vector<TypeClass *> argTypes;
    auto argBindingKinds =
        extractParamBindingKinds(node, 0, methodParent != nullptr);
    TypeClass *retType = nullptr;
    if (node->retType) {
        retType = resolveTypeNode(typeMgr, unit, node->retType);
        if (!retType) {
            error(node->loc,
                  "unknown return type for function `" +
                      toStdString(funcName) +
                      "`: " + describeTypeNode(node->retType, "void"));
        }
        rejectBareFunctionType(
            retType, node->retType,
            "unsupported bare function return type for `" +
                toStdString(funcName) + "`",
            node->loc);
        rejectOpaqueStructByValue(
            retType, node->retType, node->loc,
            "return type of function `" + toStdString(funcName) + "`");
    }

    if (methodParent) {
        argTypes.push_back(typeMgr->createPointerType(
            methodReceiverPointeeType(typeMgr, methodParent,
                                      node->receiverAccess)));
    }

    if (node->args) {
        for (auto *arg : *node->args) {
            if (!arg->is<AstVarDecl>()) {
                error(node->loc,
                      "invalid function parameter declaration in `" +
                          toStdString(funcName) + "`");
            }
            auto *varDecl = arg->as<AstVarDecl>();
            auto *type = resolveTypeNode(typeMgr, unit, varDecl->typeNode);
            if (!type) {
                error(varDecl->loc,
                      "unknown type for function parameter `" +
                          toStdString(varDecl->field) + "` in `" +
                          toStdString(funcName) +
                          "`: " + describeTypeNode(varDecl->typeNode, "void"));
            }
            rejectBareFunctionType(
                type, varDecl->typeNode,
                "unsupported bare function parameter type for `" +
                    toStdString(varDecl->field) + "` in `" +
                    toStdString(funcName) + "`",
                varDecl->loc);
            rejectOpaqueStructByValue(
                type, varDecl->typeNode, varDecl->loc,
                "parameter `" + toStdString(varDecl->field) +
                    "` in function `" + toStdString(funcName) + "`");
            argTypes.push_back(type);
        }
    }

    validateExternCFunctionSignature(node, methodParent, argTypes, retType);
    auto *funcType = typeMgr->getOrCreateFunctionType(
        argTypes, retType, std::move(argBindingKinds), node->abiKind);
    if (existingFunction) {
        auto *existingType = existingFunction->getType()->as<FuncType>();
        if (existingType != funcType ||
            existingFunction->hasImplicitSelf() != (methodParent != nullptr)) {
            reportLocalFunctionConflict(node, resolvedFunctionName,
                                        existingType, funcType);
        }
        return existingFunction;
    }
    if (methodParent && !methodParent->getMethodType(llvm::StringRef(
                            funcName.tochara(), funcName.size()))) {
        methodParent->addMethodType(
            llvm::StringRef(funcName.tochara(), funcName.size()), funcType,
            extractParamNames(node));
    }

    std::string llvmName = resolvedFunctionName.empty() ? toStdString(funcName)
                                                        : resolvedFunctionName;
    if (methodParent) {
        llvmName = resolveStructMethodSymbolName(
            methodParent,
            llvm::StringRef(funcName.tochara(), funcName.size()));
    }

    auto *llvmFunc = llvm::Function::Create(
        getFunctionAbiLLVMType(*typeMgr, funcType, methodParent != nullptr),
        llvm::Function::ExternalLinkage, llvm::Twine(llvmName),
        typeMgr->getModule());
    annotateFunctionAbi(*llvmFunc, funcType->getAbiKind());
    auto *func = new Function(llvmFunc, funcType, extractParamNames(node),
                              methodParent != nullptr);

    if (methodParent) {
        typeMgr->bindMethodFunction(
            methodParent, llvm::StringRef(funcName.tochara(), funcName.size()),
            func);
    } else {
        scope.addObj(llvm::StringRef(llvmName), func);
    }
    return func;
}

Function *
declareExtensionFunction(Scope &scope, TypeTable *typeMgr, AstFuncDecl *node,
                         CompilationUnit *unit, bool exportNamespace) {
    if (!node || !node->hasExtensionReceiver()) {
        return nullptr;
    }
    validateFunctionReceiverAccess(node, nullptr);
    if (node->isExternC()) {
        error(node->loc,
              "#[extern \"C\"] is not supported on extension methods",
              "Declare a top-level wrapper function instead.");
    }
    if (node->hasTypeParams()) {
        return nullptr;
    }

    auto receiverInfo = classifyExtensionReceiver(typeMgr, unit, node);
    auto resolvedFunctionName = resolveExtensionMethodSymbolName(
        unit, receiverInfo.receiverTypeSpelling, toStringRef(node->name),
        exportNamespace);

    Function *existingFunction = nullptr;
    if (auto *existing = scope.getObj(llvm::StringRef(resolvedFunctionName))) {
        existingFunction = existing->as<Function>();
        if (!existingFunction) {
            error(node->loc,
                  "extension method `" + resolvedFunctionName +
                      "` conflicts with an existing symbol");
        }
    }

    std::vector<TypeClass *> argTypes;
    auto argBindingKinds = extractParamBindingKinds(node);
    TypeClass *retType = nullptr;
    if (node->retType) {
        retType = resolveTypeNode(typeMgr, unit, node->retType);
        if (!retType) {
            error(node->loc,
                  "unknown return type for extension method `" +
                      toStdString(node->name) +
                      "`: " + describeTypeNode(node->retType, "void"));
        }
        rejectBareFunctionType(
            retType, node->retType,
            "unsupported bare function return type for extension method `" +
                toStdString(node->name) + "`",
            node->loc);
        rejectOpaqueStructByValue(
            retType, node->retType, node->loc,
            "return type of extension method `" + toStdString(node->name) +
                "`");
    }

    if (node->args) {
        for (auto *arg : *node->args) {
            if (!arg->is<AstVarDecl>()) {
                error(node->loc,
                      "invalid extension method parameter declaration in `" +
                          toStdString(node->name) + "`");
            }
            auto *varDecl = arg->as<AstVarDecl>();
            auto *type = resolveTypeNode(typeMgr, unit, varDecl->typeNode);
            if (!type) {
                error(varDecl->loc,
                      "unknown type for extension method parameter `" +
                          toStdString(varDecl->field) + "` in `" +
                          toStdString(node->name) +
                          "`: " + describeTypeNode(varDecl->typeNode, "void"));
            }
            rejectBareFunctionType(
                type, varDecl->typeNode,
                "unsupported bare function parameter type for `" +
                    toStdString(varDecl->field) + "` in extension method `" +
                    toStdString(node->name) + "`",
                varDecl->loc);
            rejectOpaqueStructByValue(
                type, varDecl->typeNode, varDecl->loc,
                "parameter `" + toStdString(varDecl->field) +
                    "` in extension method `" + toStdString(node->name) +
                    "`");
            argTypes.push_back(type);
        }
    }

    auto *funcType = typeMgr->getOrCreateFunctionType(
        argTypes, retType, std::move(argBindingKinds), node->abiKind);
    if (existingFunction) {
        auto *existingType = existingFunction->getType()->as<FuncType>();
        if (existingType != funcType) {
            reportLocalFunctionConflict(node, resolvedFunctionName,
                                        existingType, funcType);
        }
        return existingFunction;
    }

    auto *llvmFunc = llvm::Function::Create(
        getFunctionAbiLLVMType(*typeMgr, funcType, false),
        llvm::Function::ExternalLinkage, llvm::Twine(resolvedFunctionName),
        typeMgr->getModule());
    annotateFunctionAbi(*llvmFunc, funcType->getAbiKind());
    auto *func =
        new Function(llvmFunc, funcType, extractParamNames(node, 1), false);
    scope.addObj(llvm::StringRef(resolvedFunctionName), func);
    return func;
}

}  // namespace declarationsupport_impl
}  // namespace lona
