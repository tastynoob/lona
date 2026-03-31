#include "lona/abi/abi.hh"
#include "lona/ast/astnode.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/declare/support.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/module/module_interface.hh"
#include "lona/sema/initializer.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
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

using declarationsupport_impl::declareModuleNamespace;
using declarationsupport_impl::describeStructFieldSyntax;
using declarationsupport_impl::extractParamBindingKinds;
using declarationsupport_impl::extractParamNames;
using declarationsupport_impl::insertStructMember;
using declarationsupport_impl::interfaceMethodReceiverPointeeType;
using declarationsupport_impl::recordTopLevelDeclName;
using declarationsupport_impl::rejectBareFunctionType;
using declarationsupport_impl::rejectOpaqueStructByValue;
using declarationsupport_impl::requireTypeTable;
using declarationsupport_impl::TopLevelDeclKind;
using declarationsupport_impl::validateEmbeddedStructField;
using declarationsupport_impl::validateExternCFunctionSignature;
using declarationsupport_impl::validateFunctionReceiverAccess;
using declarationsupport_impl::validateStructDeclShape;
using declarationsupport_impl::validateStructFieldType;

namespace moduleinterface_impl {

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
    std::vector<AstTraitDecl *> traitDecls_;
    std::vector<AstTraitImplDecl *> traitImplDecls_;
    std::vector<AstFuncDecl *> funcDecls_;
    std::vector<AstGlobalDecl *> globalDecls_;
    std::unordered_map<std::string, std::pair<TopLevelDeclKind, location>>
        topLevelDecls_;

    struct ResolvedTraitRef {
        const ModuleInterface::TraitDecl *decl = nullptr;
        string resolvedName;
        bool localToUnit = false;
    };

    struct ResolvedSelfTypeRef {
        StructType *structType = nullptr;
        string resolvedName;
        bool localToUnit = false;
    };

    struct ValidatedTraitImpl {
        ModuleInterface::TraitImplDecl decl;
        location loc;
    };

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

    void validateImportAliasConflict(AstTraitDecl *traitDecl) {
        const auto name = toStdString(traitDecl->name);
        if (!unit_.importsModule(name)) {
            return;
        }
        error(traitDecl->loc,
              "trait `" + name + "` conflicts with imported module alias `" +
                  name + "`",
              "Rename the trait so `" + name +
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
        if (auto *dynType = dynamic_cast<DynTypeNode *>(node)) {
            auto *base = dynamic_cast<BaseTypeNode *>(dynType->base);
            if (!base) {
                return nullptr;
            }

            auto rawName = baseTypeName(base);
            std::string moduleName;
            std::string traitName;
            if (!splitBaseTypeName(base, moduleName, traitName)) {
                auto lookup = interface_->lookupTopLevelName(rawName);
                if (!lookup.isTrait() || !lookup.traitDecl) {
                    return nullptr;
                }
                return interface_->getOrCreateDynTraitType(
                    lookup.traitDecl->exportedName);
            }

            const auto *imported = unit_.findImportedModule(moduleName);
            if (!imported || !imported->interface) {
                return nullptr;
            }
            auto lookup = imported->interface->lookupTopLevelName(traitName);
            if (!lookup.isTrait() || !lookup.traitDecl) {
                return nullptr;
            }
            return interface_->getOrCreateDynTraitType(
                lookup.traitDecl->exportedName);
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

    static std::string describeResolvedTypeName(TypeClass *type) {
        return type ? toStdString(type->full_name) : std::string("void");
    }

    static std::string describeTraitMemberContext(AstTraitDecl *traitDecl) {
        return traitDecl ? "`" + toStdString(traitDecl->name) + "`"
                         : std::string("<trait>");
    }

    [[noreturn]] void errorUnsupportedTraitBodyStmt(AstTraitDecl *traitDecl,
                                                    AstNode *stmt) {
        const auto traitName = describeTraitMemberContext(traitDecl);
        if (auto *fieldDecl = dynamic_cast<AstVarDecl *>(stmt)) {
            error(fieldDecl->loc,
                  "trait " + traitName + " cannot declare field `" +
                      toStdString(fieldDecl->field) + "`",
                  "Trait v0 only allows method signatures inside trait "
                  "bodies.");
        }
        if (auto *varDef = dynamic_cast<AstVarDef *>(stmt)) {
            error(varDef->loc,
                  "trait " + traitName + " cannot declare local variable `" +
                      toStdString(varDef->getName()) + "`",
                  "Trait bodies describe interfaces only. Move executable "
                  "code out of the trait and keep only `def name(...)` "
                  "signatures here.");
        }
        if (auto *globalDecl = dynamic_cast<AstGlobalDecl *>(stmt)) {
            error(globalDecl->loc,
                  "trait " + traitName + " cannot declare global `" +
                      toStdString(globalDecl->getName()) + "`",
                  "Move globals to module scope. Trait v0 only allows method "
                  "signatures inside trait bodies.");
        }
        if (auto *structDecl = dynamic_cast<AstStructDecl *>(stmt)) {
            error(structDecl->loc,
                  "trait " + traitName + " cannot declare nested struct `" +
                      toStdString(structDecl->name) + "`",
                  "Move nested types to module scope. Trait bodies only allow "
                  "method signatures in trait v0.");
        }

        error(stmt ? stmt->loc : (traitDecl ? traitDecl->loc : location()),
              "trait " + traitName + " cannot contain executable statements",
              "Trait v0 only allows method signatures inside trait bodies.");
    }

    ResolvedTraitRef resolveTraitRef(AstNode *traitSyntax,
                                     const location &loc) {
        std::vector<std::string> segments;
        if (!collectDotLikeSegments(traitSyntax, segments) || segments.empty()) {
            error(loc, "invalid trait reference in impl header",
                  "Use a trait name like `Hash` or `dep.Hash` after `:`.");
        }

        if (segments.size() == 1) {
            if (const auto *traitDecl = interface_->findTrait(segments[0])) {
                return {traitDecl, traitDecl->exportedName, true};
            }
            error(loc, "unknown trait `" + segments[0] + "` in impl header",
                  "Declare the trait in this module or import the module that "
                  "defines it before writing `impl Type: Trait`.");
        }

        if (segments.size() != 2) {
            error(loc,
                  "trait references only support directly imported module "
                  "members in trait v0",
                  "Use `Trait` or `dep.Trait`. Re-exported multi-hop trait "
                  "paths are not supported yet.");
        }

        const auto *imported = unit_.findImportedModule(segments[0]);
        if (!imported || !imported->interface) {
            error(loc, "unknown imported module alias `" + segments[0] + "`",
                  "Add an explicit `import " + segments[0] +
                      "` before referring to `" + describeDotLikeSyntax(
                                                       traitSyntax, "<trait>") +
                      "`.");
        }

        auto lookup = imported->interface->lookupTopLevelName(segments[1]);
        if (!lookup.isTrait() || !lookup.traitDecl) {
            error(loc,
                  "unknown trait `" + describeDotLikeSyntax(traitSyntax,
                                                            "<trait>") +
                      "` in impl header",
                  "Only directly imported top-level traits are available "
                  "through `file.Trait`.");
        }
        return {lookup.traitDecl, lookup.traitDecl->exportedName, false};
    }

    ResolvedSelfTypeRef resolveImplSelfType(TypeNode *selfTypeNode,
                                            const location &loc) {
        auto *base = dynamic_cast<BaseTypeNode *>(selfTypeNode);
        if (!base) {
            error(loc,
                  "trait impl self type must name a concrete struct type",
                  "Write `impl Point: Hash`, not pointers, arrays, tuples, or "
                  "qualified forms.");
        }

        auto *type = resolveType(base);
        auto *structType = type ? type->as<StructType>() : nullptr;
        if (!structType) {
            error(loc,
                  "trait impl self type must resolve to a concrete struct: `" +
                      describeTypeNode(selfTypeNode, "<unknown type>") + "`",
                  "Trait v0 currently supports impls for struct types only.");
        }

        std::string moduleName;
        std::string memberName;
        const bool localToUnit =
            !splitBaseTypeName(base, moduleName, memberName);
        return {structType, structType->full_name, localToUnit};
    }

    static AccessKind inferMethodReceiverAccess(StructType *selfType,
                                                FuncType *methodType,
                                                const location &loc,
                                                llvm::StringRef methodName) {
        if (!selfType || !methodType || methodType->getArgTypes().empty()) {
            internalError(loc,
                          "trait impl validation is missing the implicit self "
                          "parameter for method `" +
                              methodName.str() + "`",
                          "This looks like a method interface bug.");
        }
        auto *selfPointeeType =
            getRawPointerPointeeType(methodType->getArgTypes().front());
        if (!selfPointeeType ||
            asUnqualified<StructType>(selfPointeeType) != selfType) {
            internalError(loc,
                          "trait impl validation found an invalid self "
                          "parameter for method `" +
                              methodName.str() + "`",
                          "This looks like a method interface bug.");
        }
        return selfPointeeType == selfType ? AccessKind::GetSet
                                           : AccessKind::GetOnly;
    }

    void validateTraitMethodMatch(const ModuleInterface::TraitDecl &traitDecl,
                                  const ModuleInterface::TraitMethodDecl &method,
                                  StructType *selfType,
                                  const location &implLoc) {
        auto *methodType = selfType->getMethodType(toStringRef(method.localName));
        const auto implLabel =
            "`" + describeResolvedType(selfType) + ": " +
            toStdString(traitDecl.exportedName) + "`";
        if (!methodType) {
            error(implLoc,
                  "impl " + implLabel + " is missing method `" +
                      toStdString(method.localName) + "`",
                  "Define `def " + toStdString(method.localName) +
                      "(...)` on `" + describeResolvedType(selfType) +
                      "` with the same signature as trait `" +
                      toStdString(traitDecl.exportedName) + "`.");
        }

        auto actualReceiverAccess = inferMethodReceiverAccess(
            selfType, methodType, implLoc, toStringRef(method.localName));
        if (actualReceiverAccess != method.receiverAccess) {
            error(
                implLoc,
                "impl " + implLabel + " has receiver access mismatch for `" +
                    toStdString(method.localName) + "`",
                "Trait `" + toStdString(traitDecl.exportedName) + "` expects `" +
                    accessKindKeyword(method.receiverAccess) + " def " +
                    toStdString(method.localName) + "`.");
        }

        const auto &argTypes = methodType->getArgTypes();
        const std::size_t actualParamCount =
            argTypes.empty() ? 0 : argTypes.size() - 1;
        if (actualParamCount != method.paramTypeSpellings.size()) {
            error(implLoc,
                  "impl " + implLabel + " has parameter count mismatch for `" +
                      toStdString(method.localName) + "`: expected " +
                      std::to_string(method.paramTypeSpellings.size()) +
                      ", got " + std::to_string(actualParamCount),
                  "Match the trait method signature exactly.");
        }

        for (std::size_t i = 0; i < actualParamCount; ++i) {
            const auto actualBindingKind = methodType->getArgBindingKind(i + 1);
            if (actualBindingKind != method.paramBindingKinds[i]) {
                error(implLoc,
                      "impl " + implLabel +
                          " has parameter binding mismatch for `" +
                          toStdString(method.localName) + "` at index " +
                          std::to_string(i),
                      "Trait `" + toStdString(traitDecl.exportedName) +
                          "` expects `" +
                          bindingKindKeyword(method.paramBindingKinds[i]) +
                          "` at that position.");
            }

            const auto actualTypeName =
                describeResolvedTypeName(argTypes[i + 1]);
            if (actualTypeName != method.paramTypeSpellings[i]) {
                error(implLoc,
                      "impl " + implLabel + " has parameter type mismatch for `" +
                          toStdString(method.localName) + "` at index " +
                          std::to_string(i) + ": expected `" +
                          toStdString(method.paramTypeSpellings[i]) + "`, got `" +
                          actualTypeName + "`",
                      "Match the trait method parameter types exactly.");
            }
        }

        const auto actualReturnTypeName =
            describeResolvedTypeName(methodType->getRetType());
        if (actualReturnTypeName != method.returnTypeSpelling) {
            error(implLoc,
                  "impl " + implLabel + " has return type mismatch for `" +
                      toStdString(method.localName) + "`: expected `" +
                      toStdString(method.returnTypeSpelling) + "`, got `" +
                      actualReturnTypeName + "`",
                  "Match the trait method return type exactly.");
        }
    }

    void checkVisibleTraitImplConflicts(
        const std::vector<ValidatedTraitImpl> &validatedImpls) {
        std::unordered_map<std::string, std::string> seenSources;

        auto makeKey = [](const string &traitName, const string &selfTypeName) {
            return toStdString(traitName) + "|" + toStdString(selfTypeName);
        };

        for (const auto &importedEntry : unit_.importedModules()) {
            const auto &alias = importedEntry.first;
            const auto &imported = importedEntry.second;
            if (!imported.interface) {
                continue;
            }
            for (const auto &implDecl : imported.interface->traitImpls()) {
                auto key = makeKey(implDecl.traitName, implDecl.selfTypeSpelling);
                auto source = "imported module `" + toStdString(alias) + "`";
                auto found = seenSources.find(key);
                if (found != seenSources.end()) {
                    error(unit_.syntaxTree() ? unit_.syntaxTree()->loc : location(),
                          "duplicate visible impl for trait `" +
                              toStdString(implDecl.traitName) + "` and type `" +
                              toStdString(implDecl.selfTypeSpelling) + "`",
                          "Only one visible `impl Type: Trait` is allowed for each "
                          "(Trait, Type) pair. Existing source: " +
                              found->second + ", new source: " + source + ".");
                }
                seenSources.emplace(std::move(key), std::move(source));
            }
        }

        for (const auto &entry : validatedImpls) {
            auto key = makeKey(entry.decl.traitName, entry.decl.selfTypeSpelling);
            auto found = seenSources.find(key);
            if (found != seenSources.end()) {
                error(entry.loc,
                      "duplicate visible impl for trait `" +
                          toStdString(entry.decl.traitName) + "` and type `" +
                          toStdString(entry.decl.selfTypeSpelling) + "`",
                      "Only one visible `impl Type: Trait` is allowed for each "
                      "(Trait, Type) pair. Existing source: " + found->second +
                          ".");
            }
            seenSources.emplace(
                std::move(key),
                "current module `" + toStdString(unit_.moduleName()) + "`");
        }
    }

    std::vector<ModuleInterface::TraitMethodDecl>
    collectTraitMethods(AstTraitDecl *traitDecl) {
        std::vector<ModuleInterface::TraitMethodDecl> methods;
        auto *body = dynamic_cast<AstStatList *>(traitDecl ? traitDecl->body
                                                           : nullptr);
        if (!body) {
            return methods;
        }

        std::unordered_map<std::string, location> seenMethods;

        for (auto *stmt : body->getBody()) {
            if (dynamic_cast<AstTagNode *>(stmt)) {
                continue;
            }
            auto *funcDecl = dynamic_cast<AstFuncDecl *>(stmt);
            if (!funcDecl) {
                errorUnsupportedTraitBodyStmt(traitDecl, stmt);
            }
            if (funcDecl->hasBody()) {
                error(funcDecl->loc,
                      "trait method `" + toStdString(funcDecl->name) +
                          "` cannot have a body in trait v0",
                      "Keep only the method signature inside the trait. "
                      "`impl Type: Trait { ... }` bodies are not supported "
                      "yet either.");
            }
            auto inserted =
                seenMethods.emplace(toStdString(funcDecl->name), funcDecl->loc);
            if (!inserted.second) {
                error(funcDecl->loc,
                      "duplicate trait method `" +
                          toStdString(funcDecl->name) + "` in trait `" +
                          toStdString(traitDecl->name) + "`",
                      "Trait method names must be unique within the same "
                      "trait.");
            }

            ModuleInterface::TraitMethodDecl method;
            method.localName = funcDecl->name;
            method.receiverAccess = funcDecl->receiverAccess;
            method.paramNames = extractParamNames(funcDecl);
            method.paramBindingKinds = extractParamBindingKinds(funcDecl);
            if (funcDecl->args) {
                method.paramTypeSpellings.reserve(funcDecl->args->size());
                for (auto *arg : *funcDecl->args) {
                    auto *varDecl = dynamic_cast<AstVarDecl *>(arg);
                    if (!varDecl) {
                        error(funcDecl->loc,
                              "invalid trait method parameter declaration in "
                              "`" +
                                  toStdString(funcDecl->name) + "`");
                    }
                    auto *paramType = resolveType(varDecl->typeNode);
                    if (!paramType) {
                        error(varDecl->loc,
                              "unknown type for trait method parameter `" +
                                  toStdString(varDecl->field) + "` in `" +
                                  toStdString(funcDecl->name) + "`: " +
                                  describeTypeNode(varDecl->typeNode, "void"));
                    }
                    rejectBareFunctionType(
                        paramType, varDecl->typeNode,
                        "unsupported bare function trait parameter type for `" +
                            toStdString(varDecl->field) + "` in `" +
                            toStdString(funcDecl->name) + "`",
                        varDecl->loc);
                    rejectOpaqueStructByValue(
                        paramType, varDecl->typeNode, varDecl->loc,
                        "parameter `" + toStdString(varDecl->field) +
                            "` in trait method `" +
                            toStdString(funcDecl->name) + "`");
                    method.paramTypeSpellings.push_back(paramType->full_name);
                }
            }
            if (funcDecl->retType) {
                auto *retType = resolveType(funcDecl->retType);
                if (!retType) {
                    error(funcDecl->loc,
                          "unknown return type for trait method `" +
                              toStdString(funcDecl->name) + "`: " +
                              describeTypeNode(funcDecl->retType, "void"));
                }
                rejectBareFunctionType(
                    retType, funcDecl->retType,
                    "unsupported bare function return type for trait method `" +
                        toStdString(funcDecl->name) + "`",
                    funcDecl->loc);
                rejectOpaqueStructByValue(
                    retType, funcDecl->retType, funcDecl->loc,
                    "return type of trait method `" +
                        toStdString(funcDecl->name) + "`");
                method.returnTypeSpelling = retType->full_name;
            } else {
                method.returnTypeSpelling = "void";
            }
            methods.push_back(std::move(method));
        }
        return methods;
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
            } else if (auto *traitDecl = dynamic_cast<AstTraitDecl *>(stmt)) {
                validateImportAliasConflict(traitDecl);
                recordTopLevelDeclName(topLevelDecls_,
                                       toStdString(traitDecl->name),
                                       TopLevelDeclKind::Trait,
                                       traitDecl->loc);
                traitDecls_.push_back(traitDecl);
            } else if (auto *traitImplDecl =
                           dynamic_cast<AstTraitImplDecl *>(stmt)) {
                traitImplDecls_.push_back(traitImplDecl);
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

    void declareTraits() {
        for (auto *traitDecl : traitDecls_) {
            auto methods = collectTraitMethods(traitDecl);
            if (!interface_->declareTrait(toStdString(traitDecl->name),
                                          std::move(methods))) {
                error(traitDecl->loc,
                      "duplicate trait `" + toStdString(traitDecl->name) + "`",
                      "Choose a distinct top-level trait name in this "
                      "module.");
            }
        }
    }

    std::vector<ValidatedTraitImpl> validateTraitImpls() {
        std::vector<ValidatedTraitImpl> validated;
        validated.reserve(traitImplDecls_.size());

        for (auto *traitImplDecl : traitImplDecls_) {
            if (traitImplDecl->hasBody()) {
                error(traitImplDecl->loc,
                      "trait impl bodies are not supported in trait v0",
                      "Declare only `impl Type: Trait` headers for now. "
                      "Keep the actual method implementations as inherent "
                      "methods on the concrete type.");
            }

            auto traitRef = resolveTraitRef(traitImplDecl->trait,
                                            traitImplDecl->loc);
            auto selfRef =
                resolveImplSelfType(traitImplDecl->selfType, traitImplDecl->loc);
            if (!traitRef.localToUnit && !selfRef.localToUnit) {
                error(traitImplDecl->loc,
                      "impl `" + toStdString(selfRef.resolvedName) + ": " +
                          toStdString(traitRef.resolvedName) +
                          "` violates the trait orphan rule",
                      "At least one side of `impl Type: Trait` must be defined "
                      "in the current module.");
            }

            for (const auto &method : traitRef.decl->methods) {
                validateTraitMethodMatch(*traitRef.decl, method,
                                         selfRef.structType,
                                         traitImplDecl->loc);
            }

            validated.push_back(ValidatedTraitImpl{
                ModuleInterface::TraitImplDecl{selfRef.resolvedName,
                                               traitRef.resolvedName, false},
                traitImplDecl->loc});
        }

        return validated;
    }

    void declareValidatedTraitImpls(
        const std::vector<ValidatedTraitImpl> &validatedImpls) {
        for (const auto &implDecl : validatedImpls) {
            interface_->declareTraitImpl(implDecl.decl.selfTypeSpelling,
                                         implDecl.decl.traitName, false);
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
        declareTraits();
        declareGlobals();
        declareFunctions();
        auto validatedTraitImpls = validateTraitImpls();
        checkVisibleTraitImplConflicts(validatedTraitImpls);
        declareValidatedTraitImpls(validatedTraitImpls);
        interface_->markCollected();
    }
};

void
ensureUnitInterfaceCollected(CompilationUnit &unit) {
    if (unit.interfaceCollected()) {
        return;
    }
    moduleinterface_impl::InterfaceCollector(unit).collect();
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

    for (const auto &entry : interface->traits()) {
        unit.bindLocalTrait(entry.first, entry.second.exportedName);
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

}  // namespace moduleinterface_impl

void
collectUnitDeclarations(Scope *global, CompilationUnit &unit,
                        bool exportNamespace) {
    moduleinterface_impl::materializeUnitInterface(global, unit,
                                                   exportNamespace);
}

}  // namespace lona
