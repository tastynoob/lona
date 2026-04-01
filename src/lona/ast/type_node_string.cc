#include "type_node_string.hh"
#include "array_dim.hh"
#include <string>

namespace lona {

std::string
describeTypeNode(const TypeNode *node, std::string_view nullDescription) {
    if (node == nullptr) {
        return std::string(nullDescription);
    }
    if (dynamic_cast<const AnyTypeNode *>(node)) {
        return "any";
    }
    if (auto *param = dynamic_cast<const FuncParamTypeNode *>(node)) {
        std::string name;
        if (param->bindingKind == BindingKind::Ref) {
            name += "ref ";
        }
        name += describeTypeNode(param->type, nullDescription);
        return name;
    }
    if (auto *applied = dynamic_cast<const AppliedTypeNode *>(node)) {
        auto name = describeTypeNode(applied->base, nullDescription);
        name += "![";
        for (size_t i = 0; i < applied->args.size(); ++i) {
            if (i != 0) {
                name += ", ";
            }
            name += describeTypeNode(applied->args[i], nullDescription);
        }
        name += "]";
        return name;
    }
    if (auto *base = dynamic_cast<const BaseTypeNode *>(node)) {
        if (base->hasSyntax()) {
            return describeDotLikeSyntax(base->syntax, nullDescription);
        }
        return toStdString(base->name);
    }
    if (auto *dynType = dynamic_cast<const DynTypeNode *>(node)) {
        return describeTypeNode(dynType->base, nullDescription) + " dyn";
    }
    if (auto *qualified = dynamic_cast<const ConstTypeNode *>(node)) {
        return describeTypeNode(qualified->base, nullDescription) + " const";
    }
    if (auto *pointer = dynamic_cast<const PointerTypeNode *>(node)) {
        auto name = describeTypeNode(pointer->base, nullDescription);
        for (uint32_t i = 0; i < pointer->dim; ++i) {
            name += "*";
        }
        return name;
    }
    if (auto *indexable =
            dynamic_cast<const IndexablePointerTypeNode *>(node)) {
        auto name = describeTypeNode(indexable->base, nullDescription);
        name += "[*]";
        return name;
    }
    if (auto *array = dynamic_cast<const ArrayTypeNode *>(node)) {
        auto name = describeTypeNode(array->base, nullDescription);
        name += describeArrayDimensions(array->dim);
        return name;
    }
    if (auto *tuple = dynamic_cast<const TupleTypeNode *>(node)) {
        std::string name = "<";
        for (size_t i = 0; i < tuple->items.size(); ++i) {
            if (i != 0) {
                name += ", ";
            }
            name += describeTypeNode(tuple->items[i], nullDescription);
        }
        name += ">";
        return name;
    }
    if (auto *func = dynamic_cast<const FuncPtrTypeNode *>(node)) {
        std::string name = "(";
        for (size_t i = 0; i < func->args.size(); ++i) {
            if (i != 0) {
                name += ", ";
            }
            name += describeTypeNode(func->args[i], nullDescription);
        }
        name += ":";
        if (func->ret) {
            name += " ";
            name += describeTypeNode(func->ret, nullDescription);
        }
        name += ")";
        return name;
    }
    return "<unknown type>";
}

}  // namespace lona
