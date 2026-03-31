#include "lona/declare/support.hh"

#include "lona/abi/abi.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/err/err.hh"

namespace lona {
namespace declarationsupport_impl {

namespace {

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

}  // namespace

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
    auto &funcName = node->name;
    auto resolvedFunctionName = resolveFunctionSymbolName(
        unit, funcName, node ? node->abiKind : AbiKind::Native,
        exportNamespace);
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
            auto *func = existing->as<Function>();
            if (!func) {
                error(node->loc, "top-level function `" +
                                     toStdString(funcName) +
                                     "` conflicts with module namespace `" +
                                     resolvedFunctionName + "`");
            }
            return func;
        }
    }

    std::vector<TypeClass *> argTypes;
    auto argBindingKinds =
        extractParamBindingKinds(node, methodParent != nullptr);
    TypeClass *retType = nullptr;
    if (node->retType) {
        retType = resolveTypeNode(typeMgr, unit, node->retType);
        if (!retType) {
            error(node->loc,
                  "unknown return type for function `" + toStdString(funcName) +
                      "`: " + describeTypeNode(node->retType, "void"));
        }
        rejectBareFunctionType(retType, node->retType,
                               "unsupported bare function return type for `" +
                                   toStdString(funcName) + "`",
                               node->loc);
        rejectOpaqueStructByValue(
            retType, node->retType, node->loc,
            "return type of function `" + toStdString(funcName) + "`");
    }

    if (methodParent) {
        argTypes.push_back(typeMgr->createPointerType(methodReceiverPointeeType(
            typeMgr, methodParent, node->receiverAccess)));
    }

    if (node->args) {
        for (auto *arg : *node->args) {
            if (!arg->is<AstVarDecl>()) {
                error(node->loc, "invalid function parameter declaration in `" +
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
    if (methodParent && !methodParent->getMethodType(llvm::StringRef(
                            funcName.tochara(), funcName.size()))) {
        methodParent->addMethodType(
            llvm::StringRef(funcName.tochara(), funcName.size()), funcType,
            extractParamNames(node));
    }

    std::string llvmName = resolvedFunctionName.empty() ? toStdString(funcName)
                                                        : resolvedFunctionName;
    if (methodParent) {
        llvmName = toStdString(methodParent->full_name) + "." + llvmName;
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

}  // namespace declarationsupport_impl
}  // namespace lona
