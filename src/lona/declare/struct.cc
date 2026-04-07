#include "lona/declare/support.hh"

#include "lona/ast/type_node_string.hh"
#include "lona/err/err.hh"

namespace lona {
namespace declarationsupport_impl {

namespace {

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
resolveTopLevelName(const CompilationUnit *unit, const string &name,
                    bool exportNamespace) {
    auto resolved = toStdString(name);
    if (!unit || !exportNamespace) {
        return resolved;
    }
    return toStdString(unit->exportNamespacePrefix() + "." + name);
}

}  // namespace

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
    (void)structDecl;
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
    (void)seenIt;
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

    string structName(resolvedName.c_str());
    auto *structType = new StructType(
        structName, node ? node->declKind : StructDeclKind::Native);

    typeMgr->addType(structName, structType);
    return structType;
}

}  // namespace declarationsupport_impl
}  // namespace lona
