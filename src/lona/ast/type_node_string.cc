#include "type_node_string.hh"
#include "array_dim.hh"
#include <string>

namespace lona {
namespace {

std::string
toStdString(const string &value) {
    return std::string(value.tochara(), value.size());
}

bool
collectFuncPtrStorageSuffixes(const TypeNode *node, const FuncPtrTypeNode *&func,
                              std::vector<std::string> &suffixes) {
    if (auto *funcNode = dynamic_cast<const FuncPtrTypeNode *>(node)) {
        func = funcNode;
        return true;
    }
    if (auto *qualified = dynamic_cast<const ConstTypeNode *>(node)) {
        if (!collectFuncPtrStorageSuffixes(qualified->base, func, suffixes)) {
            return false;
        }
        suffixes.emplace_back(" const");
        return true;
    }
    if (auto *pointer = dynamic_cast<const PointerTypeNode *>(node)) {
        if (!collectFuncPtrStorageSuffixes(pointer->base, func, suffixes)) {
            return false;
        }
        suffixes.emplace_back(pointer->dim, '*');
        return true;
    }
    if (auto *indexable = dynamic_cast<const IndexablePointerTypeNode *>(node)) {
        if (!collectFuncPtrStorageSuffixes(indexable->base, func, suffixes)) {
            return false;
        }
        suffixes.emplace_back("[*]");
        return true;
    }
    if (auto *array = dynamic_cast<const ArrayTypeNode *>(node)) {
        if (!collectFuncPtrStorageSuffixes(array->base, func, suffixes)) {
            return false;
        }
        suffixes.emplace_back(describeArrayDimensions(array->dim));
        return true;
    }
    return false;
}

std::string
describeFuncPtrStorageType(const TypeNode *node, std::string_view nullDescription) {
    const FuncPtrTypeNode *func = nullptr;
    std::vector<std::string> suffixes;
    if (!collectFuncPtrStorageSuffixes(node, func, suffixes)) {
        return {};
    }
    std::string name = "(";
    for (size_t i = 0; i < func->args.size(); ++i) {
        if (i != 0) {
            name += ", ";
        }
        name += describeTypeNode(func->args[i], nullDescription);
    }
    name += ")*";
    for (const auto &suffix : suffixes) {
        name += suffix;
    }
    if (func->ret) {
        name += " ";
        name += describeTypeNode(func->ret, nullDescription);
    }
    return name;
}

}  // namespace

std::string
describeTypeNode(const TypeNode *node, std::string_view nullDescription) {
    if (node == nullptr) {
        return std::string(nullDescription);
    }
    if (auto *param = dynamic_cast<const FuncParamTypeNode *>(node)) {
        std::string name;
        if (param->bindingKind == BindingKind::Ref) {
            name += "ref ";
        }
        name += describeTypeNode(param->type, nullDescription);
        return name;
    }
    if (auto name = describeFuncPtrStorageType(node, nullDescription); !name.empty()) {
        return name;
    }
    if (auto *base = dynamic_cast<const BaseTypeNode *>(node)) {
        return toStdString(base->name);
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
    if (auto *indexable = dynamic_cast<const IndexablePointerTypeNode *>(node)) {
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
    return "<unknown type>";
}

}  // namespace lona
