#include "type_node_tools.hh"
#include "array_dim.hh"
#include "lona/err/err.hh"
#include "tag_apply.hh"
#include "type_node_string.hh"

namespace lona {

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
validateTypeNodeLayout(const TypeNode *node) {
    if (!node) {
        return;
    }
    if (auto *param = dynamic_cast<const FuncParamTypeNode *>(node)) {
        validateTypeNodeLayout(param->type);
        return;
    }
    if (auto *qualified = dynamic_cast<const ConstTypeNode *>(node)) {
        validateTypeNodeLayout(qualified->base);
        return;
    }
    if (auto *pointer = dynamic_cast<const PointerTypeNode *>(node)) {
        if (auto *array = dynamic_cast<const ArrayTypeNode *>(pointer->base);
            array && hasUnsizedArrayDimensions(array->dim) &&
            isBareUnsizedArraySyntax(array->dim)) {
            errorLegacyTypeNodeIndexablePointerSyntax(pointer->loc, pointer);
        }
        validateTypeNodeLayout(pointer->base);
        return;
    }
    if (auto *indexable =
            dynamic_cast<const IndexablePointerTypeNode *>(node)) {
        validateTypeNodeLayout(indexable->base);
        return;
    }
    if (auto *array = dynamic_cast<const ArrayTypeNode *>(node)) {
        validateTypeNodeLayout(array->base);
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
            validateTypeNodeLayout(item);
        }
        return;
    }
    if (auto *func = dynamic_cast<const FuncPtrTypeNode *>(node)) {
        for (auto *arg : func->args) {
            validateTypeNodeLayout(arg);
        }
        validateTypeNodeLayout(func->ret);
        return;
    }
}

}  // namespace lona
