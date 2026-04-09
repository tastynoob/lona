#include "applied_struct_instantiation.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/declare/support.hh"
#include "lona/err/err.hh"
#include "lona/sema/initializer.hh"
#include "lona/type/type.hh"
#include <llvm-18/llvm/ADT/StringMap.h>
#include <llvm-18/llvm/ADT/StringSet.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lona::appliedstructinstantiation {

std::string
buildAppliedTypeName(const std::string &baseName,
                     const std::vector<TypeClass *> &args) {
    std::string name = baseName.empty() ? std::string("<type>") : baseName;
    name += "[";
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            name += ", ";
        }
        name += args[i] ? describeResolvedType(args[i]) : "<unknown type>";
    }
    name += "]";
    return name;
}

GenericArgMap
bindGenericArgs(const ModuleInterface::TypeDecl &typeDecl,
                const std::vector<TypeClass *> &appliedTypeArgs,
                const std::string &bugContext) {
    if (typeDecl.typeParams.size() != appliedTypeArgs.size()) {
        throw DiagnosticError(
            DiagnosticError::Category::Internal,
            "generic struct `" + toStdString(typeDecl.localName) + "` " +
                bugContext,
            "This looks like an applied-struct instantiation bug.");
    }

    GenericArgMap genericArgs;
    genericArgs.reserve(typeDecl.typeParams.size());
    for (std::size_t i = 0; i < typeDecl.typeParams.size(); ++i) {
        genericArgs.emplace(toStdString(typeDecl.typeParams[i].localName),
                            appliedTypeArgs[i]);
    }
    return genericArgs;
}

AstStructDecl *
findStructDeclInUnit(const CompilationUnit &unit, llvm::StringRef localName) {
    auto *root = unit.syntaxTree();
    auto *program = dynamic_cast<AstProgram *>(root);
    auto *body = dynamic_cast<AstStatList *>(program ? program->body : root);
    if (!body) {
        return nullptr;
    }
    for (auto *stmt : body->getBody()) {
        auto *structDecl = dynamic_cast<AstStructDecl *>(stmt);
        if (!structDecl) {
            continue;
        }
        if (llvm::StringRef(structDecl->name.tochara(),
                            structDecl->name.size()) == localName) {
            return structDecl;
        }
    }
    return nullptr;
}

TypeClass *
substituteTemplateType(TypeNode *node, const GenericArgMap &genericArgs,
                       const location &loc, const std::string &context,
                       const CompilationUnit &lookupUnit, const TypeOps &ops) {
    (void)context;

    if (!node) {
        return nullptr;
    }
    if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
        return substituteTemplateType(param->type, genericArgs, loc, context,
                                      lookupUnit, ops);
    }
    if (dynamic_cast<AnyTypeNode *>(node)) {
        return ops.createAnyType();
    }
    if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
        auto rawName = baseTypeName(base);
        if (auto found = genericArgs.find(rawName);
            found != genericArgs.end()) {
            return found->second;
        }
        return ops.resolveFallbackType(node, lookupUnit);
    }
    if (auto *applied = dynamic_cast<AppliedTypeNode *>(node)) {
        auto *base = dynamic_cast<BaseTypeNode *>(applied->base);
        const auto *typeDecl = ops.resolveVisibleTypeDecl(base, lookupUnit);
        if (!typeDecl) {
            return ops.resolveFallbackType(node, lookupUnit);
        }
        if (!typeDecl->isGeneric()) {
            error(applied->loc,
                  "type `" + describeTypeNode(applied, "<unknown type>") +
                      "` applies `[...]` arguments to a non-generic type",
                  "Remove the `[...]` arguments, or make the base type generic "
                  "before specializing it.");
        }
        if (applied->args.size() != typeDecl->typeParams.size()) {
            error(applied->loc,
                  "generic type argument count mismatch for `" +
                      toStdString(typeDecl->exportedName) + "`: expected " +
                      std::to_string(typeDecl->typeParams.size()) + ", got " +
                      std::to_string(applied->args.size()),
                  "Match the number of `[` `]` type arguments to the generic "
                  "type parameter list.");
        }
        std::vector<TypeClass *> argTypes;
        argTypes.reserve(applied->args.size());
        for (auto *arg : applied->args) {
            auto *argType =
                substituteTemplateType(arg, genericArgs, arg ? arg->loc : loc,
                                       context, lookupUnit, ops);
            if (!argType) {
                error(arg ? arg->loc : loc,
                      "unknown type argument for `" +
                          describeTypeNode(applied, "<unknown type>") +
                          "`: " + describeTypeNode(arg, "void"));
            }
            argTypes.push_back(argType);
        }
        return ops.instantiateAppliedStructType(*typeDecl, std::move(argTypes),
                                                lookupUnit);
    }
    if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
        auto *baseType = substituteTemplateType(qualified->base, genericArgs,
                                                loc, context, lookupUnit, ops);
        return baseType ? ops.createConstType(baseType) : nullptr;
    }
    if (auto *dynType = dynamic_cast<DynTypeNode *>(node)) {
        return ops.resolveFallbackType(dynType, lookupUnit);
    }
    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        auto *baseType = substituteTemplateType(pointer->base, genericArgs, loc,
                                                context, lookupUnit, ops);
        for (uint32_t i = 0; baseType && i < pointer->dim; ++i) {
            baseType = ops.createPointerType(baseType);
        }
        return baseType;
    }
    if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
        auto *elementType = substituteTemplateType(
            indexable->base, genericArgs, loc, context, lookupUnit, ops);
        return elementType ? ops.createIndexablePointerType(elementType)
                           : nullptr;
    }
    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        auto *elementType = substituteTemplateType(
            array->base, genericArgs, loc, context, lookupUnit, ops);
        return elementType ? ops.createArrayType(elementType, array->dim)
                           : nullptr;
    }
    if (auto *tuple = dynamic_cast<TupleTypeNode *>(node)) {
        std::vector<TypeClass *> itemTypes;
        itemTypes.reserve(tuple->items.size());
        for (auto *item : tuple->items) {
            itemTypes.push_back(substituteTemplateType(
                item, genericArgs, item ? item->loc : loc, context, lookupUnit,
                ops));
        }
        return ops.createTupleType(itemTypes);
    }
    if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
        std::vector<TypeClass *> argTypes;
        std::vector<BindingKind> argBindingKinds;
        argTypes.reserve(func->args.size());
        argBindingKinds.reserve(func->args.size());
        for (auto *arg : func->args) {
            argBindingKinds.push_back(funcParamBindingKind(arg));
            argTypes.push_back(substituteTemplateType(
                unwrapFuncParamType(arg), genericArgs, arg ? arg->loc : loc,
                context, lookupUnit, ops));
        }
        auto *retType = substituteTemplateType(func->ret, genericArgs, loc,
                                               context, lookupUnit, ops);
        return ops.createFunctionPointerType(argTypes, retType,
                                             std::move(argBindingKinds));
    }
    return ops.resolveFallbackType(node, lookupUnit);
}

void
materializeStructLayoutAndMethods(const ModuleInterface::TypeDecl &typeDecl,
                                  StructType *structType,
                                  const GenericArgMap &genericArgs,
                                  const CompilationUnit &templateOwnerUnit,
                                  const MaterializationOps &ops) {
    if (!structType) {
        return;
    }

    if (structType->isOpaque()) {
        auto *structDecl = findStructDeclInUnit(
            templateOwnerUnit, toStringRef(typeDecl.localName));
        if (!structDecl) {
            throw DiagnosticError(
                DiagnosticError::Category::Internal,
                "generic struct `" + toStdString(typeDecl.localName) +
                    "` is missing its template AST",
                "This looks like a generic template registration bug.");
        }

        auto *body = dynamic_cast<AstStatList *>(structDecl->body);
        llvm::StringMap<StructType::ValueTy> members;
        llvm::StringMap<AccessKind> memberAccess;
        llvm::StringSet<> embeddedMembers;
        std::unordered_map<std::string, location> seenMembers;
        int nextMemberIndex = 0;

        if (body) {
            for (auto *stmt : body->getBody()) {
                auto *fieldDecl = dynamic_cast<AstVarDecl *>(stmt);
                if (!fieldDecl) {
                    continue;
                }
                auto *fieldType = substituteTemplateType(
                    fieldDecl->typeNode, genericArgs, fieldDecl->loc,
                    "struct field `" +
                        declarationsupport_impl::describeStructFieldSyntax(
                            fieldDecl) +
                        "`",
                    templateOwnerUnit, ops);
                if (!fieldType) {
                    error(
                        fieldDecl->loc,
                        "unknown struct field type for `" +
                            declarationsupport_impl::describeStructFieldSyntax(
                                fieldDecl) +
                            "`: " +
                            describeTypeNode(fieldDecl->typeNode, "void"));
                }
                declarationsupport_impl::rejectBareFunctionType(
                    fieldType, fieldDecl->typeNode,
                    "unsupported bare function struct field type for `" +
                        declarationsupport_impl::describeStructFieldSyntax(
                            fieldDecl) +
                        "`",
                    fieldDecl->loc);
                declarationsupport_impl::validateStructFieldType(
                    structDecl, fieldDecl, fieldType);
                declarationsupport_impl::validateEmbeddedStructField(
                    structDecl, fieldDecl, fieldType);
                declarationsupport_impl::insertStructMember(
                    structDecl, fieldDecl, fieldType, members, memberAccess,
                    embeddedMembers, seenMembers, nextMemberIndex);
            }
        }

        structType->complete(members, memberAccess, embeddedMembers);
    }

    for (const auto &method : typeDecl.methodTemplates) {
        if (structType->getMethodType(toStringRef(method.localName))) {
            continue;
        }
        if (method.typeParams.size() > method.enclosingTypeParamCount) {
            continue;
        }

        std::vector<TypeClass *> argTypes;
        argTypes.reserve(method.paramTypeNodes.size() + 1);
        auto *selfPointee =
            ops.receiverPointeeType(structType, method.receiverAccess);
        argTypes.push_back(ops.createPointerType(selfPointee));

        for (std::size_t i = 0; i < method.paramTypeNodes.size(); ++i) {
            auto *paramType = substituteTemplateType(
                method.paramTypeNodes[i], genericArgs,
                method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                         : location(),
                "parameter `" +
                    (i < method.paramNames.size()
                         ? toStdString(method.paramNames[i])
                         : std::string("<param>")) +
                    "` in method `" + toStdString(typeDecl.localName) + "." +
                    toStdString(method.localName) + "`",
                templateOwnerUnit, ops);
            if (!paramType) {
                error(method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                               : location(),
                      "unknown method parameter type in `" +
                          toStdString(typeDecl.localName) + "." +
                          toStdString(method.localName) + "`");
            }
            declarationsupport_impl::rejectBareFunctionType(
                paramType, method.paramTypeNodes[i],
                "unsupported bare function parameter type in `" +
                    toStdString(typeDecl.localName) + "." +
                    toStdString(method.localName) + "`",
                method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                         : location());
            declarationsupport_impl::rejectOpaqueStructByValue(
                paramType, method.paramTypeNodes[i],
                method.paramTypeNodes[i] ? method.paramTypeNodes[i]->loc
                                         : location(),
                "parameter `" +
                    (i < method.paramNames.size()
                         ? toStdString(method.paramNames[i])
                         : std::string("<param>")) +
                    "` in method `" + toStdString(typeDecl.localName) + "." +
                    toStdString(method.localName) + "`");
            argTypes.push_back(paramType);
        }

        TypeClass *retType = nullptr;
        if (method.returnTypeNode) {
            retType = substituteTemplateType(
                method.returnTypeNode, genericArgs, method.returnTypeNode->loc,
                "return type of method `" + toStdString(typeDecl.localName) +
                    "." + toStdString(method.localName) + "`",
                templateOwnerUnit, ops);
            declarationsupport_impl::rejectBareFunctionType(
                retType, method.returnTypeNode,
                "unsupported bare function return type for method `" +
                    toStdString(typeDecl.localName) + "." +
                    toStdString(method.localName) + "`",
                method.returnTypeNode->loc);
            declarationsupport_impl::rejectOpaqueStructByValue(
                retType, method.returnTypeNode, method.returnTypeNode->loc,
                "return type of method `" + toStdString(typeDecl.localName) +
                    "." + toStdString(method.localName) + "`");
        }

        auto paramBindingKinds = method.paramBindingKinds;
        paramBindingKinds.insert(paramBindingKinds.begin(), BindingKind::Value);
        auto *funcType =
            ops.createMethodFunctionType(argTypes, retType, paramBindingKinds);
        structType->addMethodType(toStringRef(method.localName), funcType,
                                  method.paramNames);
    }
}

}  // namespace lona::appliedstructinstantiation
