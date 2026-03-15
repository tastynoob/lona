#include "astnode.hh"

namespace lona {

namespace {

std::string
typeNodeToString(TypeNode *node) {
    if (node == nullptr) {
        return "<unknown type>";
    }
    if (auto *base = dynamic_cast<BaseTypeNode *>(node)) {
        return std::string(base->name.tochara(), base->name.size());
    }
    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        auto name = typeNodeToString(pointer->base);
        for (uint32_t i = 0; i < pointer->dim; ++i) {
            name += "*";
        }
        return name;
    }
    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        auto name = typeNodeToString(array->base);
        name += "[";
        for (size_t i = 0; i < array->dim.size(); ++i) {
            if (i != 0) {
                name += ",";
            }
            if (array->dim[i] != nullptr) {
                name += "?";
            }
        }
        name += "]";
        return name;
    }
    if (auto *func = dynamic_cast<FuncTypeNode *>(node)) {
        std::string name = "(";
        for (size_t i = 0; i < func->args.size(); ++i) {
            if (i != 0) {
                name += ", ";
            }
            name += typeNodeToString(func->args[i]);
        }
        name += ")";
        if (func->ret) {
            name += " ";
            name += typeNodeToString(func->ret);
        }
        return name;
    }
    return "<unknown type>";
}

}  // namespace

void
AstProgram::toJson(Json &root) {
    root["type"] = "Program";
    root["body"] = Json::object();
    this->body->toJson(root["body"]);
}

void
AstConst::toJson(Json &root) {
    switch (this->vtype) {
        case Type::INT32:
            root["type"] = "const";
            root["value"] = *(int32_t *)this->buf;
            break;
        case Type::FP32:
            root["type"] = "const";
            root["value"] = *(float *)this->buf;
            break;
        case Type::STRING:
            root["type"] = "const";
            root["value"] = (char *)this->buf;
            break;
        case Type::BOOL:
            root["type"] = "const";
            root["value"] = *(bool *)this->buf;
            break;
        default:
            throw std::runtime_error("Invalid type for AstConst");
    }
}

void
AstField::toJson(Json &root) {
    root["type"] = "field";
    root["name"] = this->name.tochara();
}

void
AstFuncRef::toJson(Json &root) {
    root["type"] = "FuncRef";
    root["name"] = this->name.tochara();
    root["args"] = Json::array();
    if (argTypes) {
        for (auto *argType : *argTypes) {
            root["args"].push_back(typeNodeToString(argType));
        }
    }
}

void
AstAssign::toJson(Json &root) {
    root["type"] = "Assign";
    root["left"] = Json::object();
    this->left->toJson(root["left"]);
    root["right"] = Json::object();
    this->right->toJson(root["right"]);
}

void
AstBinOper::toJson(Json &root) {
    root["type"] = "BinaryOperator";
    root["op"] = symbolToStr(this->op).tochara();
    root["left"] = Json::object();
    this->left->toJson(root["left"]);
    root["right"] = Json::object();
    this->right->toJson(root["right"]);
}

void
AstUnaryOper::toJson(Json &root) {
    root["type"] = "UnaryOperator";
    root["op"] = symbolToStr(this->op).tochara();
    root["expr"] = Json::object();
    this->expr->toJson(root["expr"]);
}

void
AstStructDecl::toJson(Json &root) {
    root["type"] = "StructDecl";
    root["name"] = this->name.tochara();
    root["body"] = Json::object();
    this->body->toJson(root["body"]);
}

void
AstImport::toJson(Json &root) {
    root["type"] = "Import";
    root["path"] = path;
}

void
AstVarDecl::toJson(Json &root) {
    root["type"] = "VarDecl";
    root["field"] = this->field.tochara();
    // if (typeHelper) {
    //     root["typestr"] = typeHelper->toString();
    // }
    if (right) {
        root["right"] = Json::object();
        this->right->toJson(root["right"]);
    }
}

void
AstVarDef::toJson(Json &root) {
    root["type"] = "VarDef";
    root["field"] = this->field.tochara();
    if (this->initVal != nullptr) {
        root["init"] = Json::object();
        this->initVal->toJson(root["init"]);
    }
}

void
AstStatList::toJson(Json &root) {
    root["type"] = "StatList";
    root["body"] = Json::array();
    for (auto it : this->body) {
        Json stat = Json::object();
        it->toJson(stat);
        root["body"].push_back(stat);
    }
}

void
AstFuncDecl::toJson(Json &root) {
    root["type"] = "FuncDecl";
    root["name"] = this->name.tochara();
    // if (this->retType) root["ret"] = this->retType->toString();
    if (args) {
        root["args"] = Json::array();
        for (auto &it : *this->args) {
            root["args"].push_back(Json::object());
            it->toJson(root["args"].back());
        }
    } else {
        root["args"] = "none";
    }
    root["body"] = Json::object();
    this->body->toJson(root["body"]);
}

void
AstRet::toJson(Json &root) {
    root["type"] = "Return";
    if (this->expr != nullptr) {
        root["value"] = Json::object();
        this->expr->toJson(root["value"]);
    } else {
        root["value"] = nullptr;
    }
}

void
AstIf::toJson(Json &root) {
    root["type"] = "If";
    root["cond"] = Json::object();
    this->condition->toJson(root["cond"]);
    root["then"] = Json::object();
    this->then->toJson(root["then"]);
    if (this->els != nullptr) {
        root["else"] = Json::object();
        this->els->toJson(root["else"]);
    }
}

void
AstFor::toJson(Json &root) {
    root["type"] = "For";
    root["cond"] = Json::object();
    this->expr->toJson(root["cond"]);
    root["body"] = Json::object();
    this->body->toJson(root["body"]);
}

void
AstFieldCall::toJson(Json &root) {
    root["type"] = "FieldCall";
    root["value"] = Json::object();
    this->value->toJson(root["value"]);
    if (this->args) {
        root["args"] = Json::array();
        for (auto &it : *this->args) {
            Json arg = Json::object();
            it->toJson(arg);
            root["args"].push_back(arg);
        }
    } else {
        root["args"] = "none";
    }
}

void
AstSelector::toJson(Json &root) {
    root["type"] = "Selector";
    root["parent"] = Json::object();
    this->parent->toJson(root["parent"]);
    root["field"] = this->field->text.tochara();
}

}  // namespace lona
