#include "type_node_tools.hh"
#include "array_dim.hh"
#include "lona/err/err.hh"
#include "tag_apply.hh"
#include "type_node_string.hh"

namespace lona {

namespace {

bool
isBuiltinTypeNameText(llvm::StringRef name) {
    return name == "u8" || name == "i8" || name == "u16" || name == "i16" ||
           name == "u32" || name == "i32" || name == "u64" || name == "i64" ||
           name == "usize" || name == "int" || name == "uint" ||
           name == "f32" || name == "f64" || name == "bool";
}

const BaseTypeNode *
rootBaseTypeNode(const TypeNode *node) {
    if (!node) {
        return nullptr;
    }
    if (auto *base = dynamic_cast<const BaseTypeNode *>(node)) {
        return base;
    }
    if (auto *applied = dynamic_cast<const AppliedTypeNode *>(node)) {
        return rootBaseTypeNode(applied->base);
    }
    return nullptr;
}

bool
isNamedTypeConstructorBase(const TypeNode *node) {
    auto *root = rootBaseTypeNode(node);
    if (!root) {
        return false;
    }
    auto rawName = baseTypeName(root);
    return !rawName.empty() &&
           !isBuiltinTypeNameText(
               llvm::StringRef(rawName.c_str(), rawName.size()));
}

}  // namespace

std::string
baseTypeName(const BaseTypeNode *node) {
    if (!node) {
        return {};
    }
    if (node->hasSyntax()) {
        return describeDotLikeSyntax(node->syntax);
    }
    return toStdString(node->name);
}

bool
splitBaseTypeName(const BaseTypeNode *node, std::string &moduleName,
                  std::string &memberName) {
    if (!node) {
        return false;
    }
    if (node->hasSyntax()) {
        std::vector<std::string> segments;
        if (!collectDotLikeSegments(node->syntax, segments) ||
            segments.size() < 2) {
            return false;
        }

        moduleName = segments.front();
        memberName.clear();
        for (std::size_t i = 1; i < segments.size(); ++i) {
            if (!memberName.empty()) {
                memberName += ".";
            }
            memberName += segments[i];
        }
        return true;
    }

    auto rawName = baseTypeName(node);
    auto separator = rawName.find('.');
    if (separator == std::string::npos) {
        return false;
    }
    moduleName = rawName.substr(0, separator);
    memberName = rawName.substr(separator + 1);
    return true;
}

bool
isReservedInitialListTypeName(llvm::StringRef name) {
    return name == "initial_list";
}

bool
isReservedInitialListTypeNode(TypeNode *node) {
    auto *base = dynamic_cast<BaseTypeNode *>(node);
    if (!base) {
        return false;
    }
    return isReservedInitialListTypeName(baseTypeName(base));
}

[[noreturn]] void
errorReservedInitialListType(const location &loc) {
    error(loc, "`initial_list` is a compiler-internal initialization interface",
          "Use brace initialization like `{1, 2, 3}` instead. User-visible "
          "generic `initial_list<T>` support is not implemented.");
}

[[noreturn]] void
errorPointerOnlyAnyType(const location &loc, const TypeNode *node) {
    error(loc,
          "bare `any` is not a value type in generic v0: " +
              describeTypeNode(node, "<unknown type>"),
          "Use `any*`, `any const*`, `any[*]`, or `any const[*]` at an "
          "explicit erased-pointer boundary instead.");
}

TypeNode *
typeNodeFromBracketItem(AstNode *node) {
    if (!node) {
        return nullptr;
    }
    if (auto *field = dynamic_cast<AstField *>(node)) {
        if (field->name == string("any")) {
            return new AnyTypeNode(node->loc);
        }
        return new BaseTypeNode(node, field->loc);
    }
    if (dynamic_cast<AstDotLike *>(node)) {
        return new BaseTypeNode(node, node->loc);
    }
    if (auto *applied = dynamic_cast<AstTypeApply *>(node)) {
        auto *base = typeNodeFromBracketItem(applied->value);
        if (!base) {
            return nullptr;
        }
        return new AppliedTypeNode(
            base, applied->typeArgs ? *applied->typeArgs
                                    : std::vector<TypeNode *>{},
            applied->loc);
    }
    return nullptr;
}

TypeNode *
createBracketSuffixTypeNode(TypeNode *base, std::vector<AstNode *> *items,
                            const location &loc) {
    if (!items) {
        return new ArrayTypeNode(base, {}, loc);
    }

    if (isNamedTypeConstructorBase(base) && !items->empty()) {
        std::vector<TypeNode *> typeArgs;
        typeArgs.reserve(items->size());
        bool allTypeArgs = true;
        for (auto *item : *items) {
            auto *typeArg = typeNodeFromBracketItem(item);
            if (!typeArg) {
                allTypeArgs = false;
                break;
            }
            typeArgs.push_back(typeArg);
        }
        if (allTypeArgs) {
            return new AppliedTypeNode(base, std::move(typeArgs), loc);
        }
        for (auto *typeArg : typeArgs) {
            delete typeArg;
        }
    }

    return new ArrayTypeNode(base, *items, loc);
}

[[noreturn]] void
errorInvalidTypeNodeArrayDimension(const location &loc) {
    error(loc, "fixed-dimension arrays require positive integer literal sizes",
          "Use explicit sizes like `i32[4][5]` or `i32[5,4]`. Dimension "
          "inference and non-constant sizes are not implemented yet.");
}

[[noreturn]] void
errorUnsupportedTypeNodeUnsizedArray(const location &loc,
                                     const TypeNode *node) {
    error(loc,
          "explicit unsized array type syntax is not allowed: " +
              describeTypeNode(node, "<unknown type>"),
          "Use fixed explicit dimensions like `i32[2]`. If you want inferred "
          "array dimensions, write `var a = {1, 2}`. If you need an indexable "
          "pointer, write `T[*]`.");
}

[[noreturn]] void
errorLegacyDynConstTypeSyntax(const location &loc) {
    error(loc, "read-only trait objects use `Trait const dyn`, not `Trait dyn const`",
          "Write `Hash const dyn` instead of `Hash dyn const`.");
}

[[noreturn]] void
errorLegacyTypeNodeIndexablePointerSyntax(const location &loc,
                                          const TypeNode *node) {
    error(loc,
          "explicit unsized array type syntax is not allowed inside pointer "
          "declarations: " +
              describeTypeNode(node, "<unknown type>"),
          "Use `T[*]` instead, for example `u8[*]`. `[]` is not a "
          "user-writable type declaration syntax.");
}

void
validateTypeNodeLayoutImpl(const TypeNode *node, bool allowDirectAny) {
    if (!node) {
        return;
    }
    if (dynamic_cast<const AnyTypeNode *>(node)) {
        if (!allowDirectAny) {
            errorPointerOnlyAnyType(node->loc, node);
        }
        return;
    }
    if (auto *param = dynamic_cast<const FuncParamTypeNode *>(node)) {
        validateTypeNodeLayoutImpl(param->type, false);
        return;
    }
    if (auto *applied = dynamic_cast<const AppliedTypeNode *>(node)) {
        validateTypeNodeLayoutImpl(applied->base, false);
        for (auto *arg : applied->args) {
            validateTypeNodeLayoutImpl(arg, false);
        }
        return;
    }
    if (auto *qualified = dynamic_cast<const ConstTypeNode *>(node)) {
        if (dynamic_cast<const DynTypeNode *>(qualified->base)) {
            errorLegacyDynConstTypeSyntax(node->loc);
        }
        validateTypeNodeLayoutImpl(qualified->base, allowDirectAny);
        return;
    }
    if (auto *dynType = dynamic_cast<const DynTypeNode *>(node)) {
        validateTypeNodeLayoutImpl(dynType->base, false);
        return;
    }
    if (auto *pointer = dynamic_cast<const PointerTypeNode *>(node)) {
        if (auto *array = dynamic_cast<const ArrayTypeNode *>(pointer->base);
            array && hasUnsizedArrayDimensions(array->dim) &&
            isBareUnsizedArraySyntax(array->dim)) {
            errorLegacyTypeNodeIndexablePointerSyntax(pointer->loc, pointer);
        }
        validateTypeNodeLayoutImpl(pointer->base, true);
        return;
    }
    if (auto *indexable =
            dynamic_cast<const IndexablePointerTypeNode *>(node)) {
        validateTypeNodeLayoutImpl(indexable->base, true);
        return;
    }
    if (auto *array = dynamic_cast<const ArrayTypeNode *>(node)) {
        validateTypeNodeLayoutImpl(array->base, false);
        if (hasUnsizedArrayDimensions(array->dim)) {
            errorUnsupportedTypeNodeUnsizedArray(array->loc, array);
        }
        for (auto *dimension : array->dim) {
            std::int64_t value = 0;
            if (!tryExtractArrayDimension(dimension, value) || value <= 0) {
                errorInvalidTypeNodeArrayDimension(dimension ? dimension->loc
                                                             : array->loc);
            }
        }
        return;
    }
    if (auto *tuple = dynamic_cast<const TupleTypeNode *>(node)) {
        for (auto *item : tuple->items) {
            validateTypeNodeLayoutImpl(item, false);
        }
        return;
    }
    if (auto *func = dynamic_cast<const FuncPtrTypeNode *>(node)) {
        for (auto *arg : func->args) {
            validateTypeNodeLayoutImpl(arg, false);
        }
        validateTypeNodeLayoutImpl(func->ret, false);
        return;
    }
}

const BaseTypeNode *
getDynTraitBaseNode(const DynTypeNode *node, bool *readOnlyDataPtr) {
    if (readOnlyDataPtr) {
        *readOnlyDataPtr = false;
    }
    if (!node || !node->base) {
        return nullptr;
    }

    auto *base = node->base;
    if (auto *qualified = dynamic_cast<const ConstTypeNode *>(base)) {
        if (readOnlyDataPtr) {
            *readOnlyDataPtr = true;
        }
        base = qualified->base;
    }
    return dynamic_cast<const BaseTypeNode *>(base);
}

void
validateTypeNodeLayout(const TypeNode *node) {
    validateTypeNodeLayoutImpl(node, false);
}

}  // namespace lona
