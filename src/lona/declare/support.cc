#include "lona/declare/support.hh"

#include "lona/ast/type_node_string.hh"
#include "lona/ast/type_node_tools.hh"
#include "lona/err/err.hh"
#include <cassert>

namespace lona {
namespace declarationsupport_impl {

namespace {

const char *
topLevelDeclKindName(TopLevelDeclKind kind) {
    switch (kind) {
        case TopLevelDeclKind::StructType:
            return "struct";
        case TopLevelDeclKind::Trait:
            return "trait";
        case TopLevelDeclKind::Function:
            return "top-level function";
        case TopLevelDeclKind::Global:
            return "global";
        case TopLevelDeclKind::Inline:
            return "inline constant";
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

}  // namespace

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

TypeClass *
resolveTypeNode(TypeTable *typeMgr, const CompilationUnit *unit,
                TypeNode *node) {
    if (!typeMgr) {
        return nullptr;
    }
    validateTypeNodeLayout(node);
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

}  // namespace declarationsupport_impl
}  // namespace lona
