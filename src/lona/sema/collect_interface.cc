#include "../abi/abi.hh"
#include "../type/buildin.hh"
#include "../type/scope.hh"
#include "lona/ast/astnode.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/module/module_interface.hh"
#include "lona/sema/collect_internal.hh"
#include "lona/sema/initializer_semantics.hh"
#include <cassert>
#include <cstdint>
#include <llvm-18/llvm/IR/Function.h>
#include <llvm-18/llvm/IR/GlobalVariable.h>
#include <llvm-18/llvm/IR/Module.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lona {

using collect_decl_impl::declareModuleNamespace;
using collect_decl_impl::describeStructFieldSyntax;
using collect_decl_impl::extractParamBindingKinds;
using collect_decl_impl::extractParamNames;
using collect_decl_impl::insertStructMember;
using collect_decl_impl::interfaceMethodReceiverPointeeType;
using collect_decl_impl::recordTopLevelDeclName;
using collect_decl_impl::rejectBareFunctionType;
using collect_decl_impl::rejectOpaqueStructByValue;
using collect_decl_impl::requireTypeTable;
using collect_decl_impl::TopLevelDeclKind;
using collect_decl_impl::validateEmbeddedStructField;
using collect_decl_impl::validateExternCFunctionSignature;
using collect_decl_impl::validateFunctionReceiverAccess;
using collect_decl_impl::validateStructDeclShape;
using collect_decl_impl::validateStructFieldType;

namespace collect_interface_impl {

[[noreturn]] void
reportGlobalConflict(const CompilationUnit *unit, llvm::StringRef globalName,
                     TypeClass *existingType, TypeClass *incomingType) {
    std::string message =
        "conflicting declarations for global `" + globalName.str() + "`";
    if (existingType && incomingType) {
        message += ": `" + describeResolvedType(existingType) + "` vs `" +
                   describeResolvedType(incomingType) + "`";
    }

    std::string hint =
        "Make duplicated `#[extern] global` declarations use the same type in "
        "every module.";
    if (unit && unit->syntaxTree()) {
        throw DiagnosticError(DiagnosticError::Category::Semantic,
                              unit->syntaxTree()->loc, std::move(message),
                              std::move(hint));
    }
    throw DiagnosticError(DiagnosticError::Category::Semantic,
                          std::move(message), std::move(hint));
}

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
    if (name == "usize") return usizeTy;
    if (name == "int") return i32Ty;
    if (name == "uint") return u32Ty;
    if (name == "f32") return f32Ty;
    if (name == "f64") return f64Ty;
    if (name == "bool") return boolTy;
    return nullptr;
}

class InterfaceCollector {
    CompilationUnit &unit_;
    ModuleInterface *interface_;
    std::vector<AstStructDecl *> structDecls_;
    std::vector<AstFuncDecl *> funcDecls_;
    std::vector<AstGlobalDecl *> globalDecls_;
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

    void validateImportAliasConflict(AstGlobalDecl *globalDecl) {
        const auto name = toStdString(globalDecl->getName());
        if (!unit_.importsModule(name)) {
            return;
        }
        error(globalDecl->loc,
              "global `" + name + "` conflicts with imported module alias `" +
                  name + "`",
              "Rename the global so `" + name +
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
            return baseType ? interface_->getOrCreateConstType(baseType)
                            : nullptr;
        }
        if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
            auto rawName = baseTypeName(base);
            std::string moduleName;
            std::string typeName;
            if (!splitBaseTypeName(base, moduleName, typeName)) {
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

            const auto *imported = unit_.findImportedModule(moduleName);
            if (!imported || !imported->interface) {
                return nullptr;
            }
            auto lookup = imported->interface->lookupTopLevelName(typeName);
            return lookup.isType() && lookup.typeDecl ? lookup.typeDecl->type
                                                      : nullptr;
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
            return elementType ? interface_->getOrCreateIndexablePointerType(
                                     elementType)
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
        auto *body =
            dynamic_cast<AstStatList *>(program ? program->body : root);
        if (!body) {
            return;
        }
        for (auto *stmt : body->getBody()) {
            if (auto *structDecl = dynamic_cast<AstStructDecl *>(stmt)) {
                validateImportAliasConflict(structDecl);
                validateStructDeclShape(structDecl);
                recordTopLevelDeclName(
                    topLevelDecls_, toStdString(structDecl->name),
                    TopLevelDeclKind::StructType, structDecl->loc);
                structDecls_.push_back(structDecl);
            } else if (auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt)) {
                validateImportAliasConflict(funcDecl);
                recordTopLevelDeclName(
                    topLevelDecls_, toStdString(funcDecl->name),
                    TopLevelDeclKind::Function, funcDecl->loc);
                funcDecls_.push_back(funcDecl);
            } else if (auto *globalDecl = dynamic_cast<AstGlobalDecl *>(stmt)) {
                validateImportAliasConflict(globalDecl);
                recordTopLevelDeclName(
                    topLevelDecls_, toStdString(globalDecl->getName()),
                    TopLevelDeclKind::Global, globalDecl->loc);
                globalDecls_.push_back(globalDecl);
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
            auto *typeDecl =
                interface_->findType(toStdString(structDecl->name));
            auto *structType =
                typeDecl ? typeDecl->type->as<StructType>() : nullptr;
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
            llvm::StringMap<AccessKind> memberAccess;
            llvm::StringSet<> embeddedMembers;
            std::unordered_map<std::string, location> seenMembers;
            int index = 0;
            for (auto *stmt : body->getBody()) {
                auto *varDecl = dynamic_cast<AstVarDecl *>(stmt);
                if (!varDecl) {
                    continue;
                }
                if (varDecl->bindingKind == BindingKind::Ref) {
                    error(varDecl->loc,
                          "struct fields cannot use `ref` binding for `" +
                              describeStructFieldSyntax(varDecl) + "`",
                          "Store an explicit pointer type instead. Struct "
                          "fields must be value or pointer-like storage.");
                }
                auto *fieldType = resolveType(varDecl->typeNode);
                if (!fieldType) {
                    error(varDecl->loc,
                          "unknown struct field type for `" +
                              describeStructFieldSyntax(varDecl) + "`: " +
                              describeTypeNode(varDecl->typeNode, "void"));
                }
                rejectBareFunctionType(
                    fieldType, varDecl->typeNode,
                    "unsupported bare function struct field type for `" +
                        describeStructFieldSyntax(varDecl) + "`",
                    varDecl->loc);
                validateStructFieldType(structDecl, varDecl, fieldType);
                validateEmbeddedStructField(structDecl, varDecl, fieldType);
                insertStructMember(structDecl, varDecl, fieldType, members,
                                   memberAccess, embeddedMembers, seenMembers,
                                   index);
            }

            structType->complete(members, memberAccess, embeddedMembers);
        }
    }

    FuncType *buildFunctionType(AstFuncDecl *node, StructType *methodParent) {
        validateFunctionReceiverAccess(node, methodParent);
        std::vector<TypeClass *> argTypes;
        auto argBindingKinds =
            extractParamBindingKinds(node, methodParent != nullptr);
        if (methodParent) {
            argTypes.push_back(interface_->getOrCreatePointerType(
                interfaceMethodReceiverPointeeType(interface_, methodParent,
                                                   node->receiverAccess)));
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
                error(node->loc, "unknown return type for function `" +
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
        return interface_->getOrCreateFunctionType(
            argTypes, retType, std::move(argBindingKinds), node->abiKind);
    }

    void declareFunctions() {
        for (auto *structDecl : structDecls_) {
            auto *typeDecl =
                interface_->findType(toStdString(structDecl->name));
            auto *structType =
                typeDecl ? typeDecl->type->as<StructType>() : nullptr;
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
                if (structType->getMethodType(llvm::StringRef(
                        funcDecl->name.tochara(), funcDecl->name.size())) ==
                    nullptr) {
                    structType->addMethodType(
                        llvm::StringRef(funcDecl->name.tochara(),
                                        funcDecl->name.size()),
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

    void declareGlobals() {
        for (auto *globalDecl : globalDecls_) {
            TypeClass *globalType = nullptr;
            if (globalDecl->hasTypeNode()) {
                globalType = resolveType(globalDecl->getTypeNode());
                if (!globalType) {
                    error(globalDecl->loc,
                          "unknown type for global `" +
                              toStdString(globalDecl->getName()) + "`: " +
                              describeTypeNode(globalDecl->getTypeNode(),
                                               "void"));
                }
                rejectBareFunctionType(
                    globalType, globalDecl->getTypeNode(),
                    "unsupported bare function global type for `" +
                        toStdString(globalDecl->getName()) + "`",
                    globalDecl->loc);
                rejectOpaqueStructByValue(
                    globalType, globalDecl->getTypeNode(), globalDecl->loc,
                    "global `" + toStdString(globalDecl->getName()) + "`");
            } else {
                globalType = inferStaticLiteralInitializerType(
                    interface_, globalDecl->getInitVal());
                if (!globalType) {
                    if (auto *constant =
                            dynamic_cast<AstConst *>(globalDecl->getInitVal());
                        constant &&
                        constant->getType() == AstConst::Type::NULLPTR) {
                        error(globalDecl->loc,
                              "global `" + toStdString(globalDecl->getName()) +
                                  "` cannot infer a type from `null`",
                              "Add an explicit pointer type, for example "
                              "`global " +
                                  toStdString(globalDecl->getName()) +
                                  " u8* = null`.");
                    }
                    error(globalDecl->loc,
                          "global `" + toStdString(globalDecl->getName()) +
                              "` requires a statically typed initializer for "
                              "inference",
                          "This first version infers global types only from "
                          "literal initializers such as numbers, booleans, "
                          "chars, and strings. Add an explicit type for other "
                          "cases.");
                }
            }

            const auto globalName = toStdString(globalDecl->getName());
            if (!interface_->declareGlobal(globalName, globalType,
                                           globalDecl->isExtern())) {
                error(globalDecl->loc, "duplicate global `" + globalName + "`",
                      "Choose a distinct top-level global name in this "
                      "module.");
            }
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
        declareGlobals();
        declareFunctions();
        interface_->markCollected();
    }
};

void
ensureUnitInterfaceCollected(CompilationUnit &unit) {
    if (unit.interfaceCollected()) {
        return;
    }
    collect_interface_impl::InterfaceCollector(unit).collect();
}

Function *
materializeDeclaredFunction(Scope &scope, TypeTable *typeMgr,
                            FuncType *funcType, llvm::StringRef llvmName,
                            std::vector<string> paramNames = {},
                            bool hasImplicitSelf = false) {
    auto *existing = scope.getObj(llvmName);
    if (existing) {
        return existing->as<Function>();
    }
    auto *llvmFunc = llvm::Function::Create(
        getFunctionAbiLLVMType(*typeMgr, funcType, hasImplicitSelf),
        llvm::Function::ExternalLinkage, llvm::Twine(llvmName),
        typeMgr->getModule());
    annotateFunctionAbi(*llvmFunc, funcType->getAbiKind());
    auto *func = new Function(llvmFunc, funcType, std::move(paramNames),
                              hasImplicitSelf);
    scope.addObj(llvmName, func);
    return func;
}

Object *
materializeDeclaredGlobal(Scope &scope, TypeTable *typeMgr, TypeClass *type,
                          llvm::StringRef llvmName,
                          const CompilationUnit *unit = nullptr) {
    if (!type) {
        internalError("failed to materialize global declaration `" +
                          llvmName.str() + "` without a type",
                      "Global interfaces should only contain resolved types.");
    }

    if (auto *existing = scope.getObj(llvmName)) {
        if (existing->getType() != type) {
            reportGlobalConflict(unit, llvmName, existing->getType(), type);
        }
        return existing;
    }

    auto *llvmGlobal = scope.module.getGlobalVariable(llvmName);
    auto *llvmType = typeMgr->getLLVMType(type);
    if (llvmGlobal == nullptr) {
        llvmGlobal = new llvm::GlobalVariable(
            scope.module, llvmType, !isFullyWritableValueType(type),
            llvm::GlobalValue::ExternalLinkage, nullptr, llvmName);
    } else if (llvmGlobal->getValueType() != llvmType) {
        reportGlobalConflict(unit, llvmName, nullptr, type);
    }

    auto *obj = type->newObj(Object::VARIABLE);
    obj->setllvmValue(llvmGlobal);
    scope.addObj(llvmName, obj);
    return obj;
}

void
materializeUnitInterface(Scope *global, CompilationUnit &unit,
                         bool exportNamespace) {
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
            internalError(
                "failed to materialize imported type `" +
                    toStdString(entry.first) + "` from module `" +
                    toStdString(unit.path()) + "`",
                "Imported interfaces should only contain types that were "
                "successfully collected from the defining module.");
        }
        unit.bindLocalType(entry.first, toStdString(type->full_name));
    }

    for (const auto &entry : interface->functions()) {
        auto *storedType = typeMgr->internType(entry.second.type);
        auto *funcType = storedType ? storedType->as<FuncType>() : nullptr;
        if (!funcType) {
            internalError(
                "failed to materialize imported function signature `" +
                    toStdString(entry.first) + "` from module `" +
                    toStdString(unit.path()) + "`",
                "Imported interfaces should only contain function signatures "
                "that were successfully collected from the defining module.");
        }
        auto runtimeName =
            exportNamespace ? entry.second.symbolName : entry.first;
        unit.bindLocalFunction(entry.first, runtimeName);
        materializeDeclaredFunction(*global, typeMgr, funcType,
                                    toStringRef(runtimeName),
                                    entry.second.paramNames);
    }

    for (const auto &entry : interface->globals()) {
        auto *storedType = typeMgr->internType(entry.second.type);
        if (!storedType) {
            internalError("failed to materialize imported global `" +
                              toStdString(entry.first) + "` from module `" +
                              toStdString(unit.path()) + "`",
                          "Imported interfaces should only contain globals "
                          "with fully resolved types.");
        }
        auto runtimeName =
            exportNamespace ? entry.second.symbolName : entry.first;
        unit.bindLocalGlobal(entry.first, runtimeName);
        materializeDeclaredGlobal(*global, typeMgr, storedType,
                                  toStringRef(runtimeName), &unit);
    }

    for (const auto &entry : interface->types()) {
        auto *storedType = typeMgr->internType(entry.second.type);
        auto *structType = storedType ? storedType->as<StructType>() : nullptr;
        if (!structType) {
            continue;
        }
        for (const auto &method : structType->getMethodTypes()) {
            auto *storedMethodType = typeMgr->internType(method.second);
            auto *methodType =
                storedMethodType ? storedMethodType->as<FuncType>() : nullptr;
            if (!methodType) {
                continue;
            }
            if (typeMgr->getMethodFunction(structType, method.first())) {
                continue;
            }
            auto methodName =
                toStdString(structType->full_name) + "." + method.first().str();
            auto *llvmFunc = llvm::Function::Create(
                getFunctionAbiLLVMType(*typeMgr, methodType, true),
                llvm::Function::ExternalLinkage, llvm::Twine(methodName),
                typeMgr->getModule());
            annotateFunctionAbi(*llvmFunc, methodType->getAbiKind());
            std::vector<string> paramNames;
            if (const auto *storedParamNames =
                    structType->getMethodParamNames(method.first())) {
                paramNames = *storedParamNames;
            }
            typeMgr->bindMethodFunction(
                structType, method.first(),
                new Function(llvmFunc, methodType, std::move(paramNames),
                             true));
        }
    }

    unit.markInterfaceCollected();
}

}  // namespace collect_interface_impl

void
collectUnitDeclarations(Scope *global, CompilationUnit &unit,
                        bool exportNamespace) {
    collect_interface_impl::materializeUnitInterface(global, unit,
                                                     exportNamespace);
}

}  // namespace lona
