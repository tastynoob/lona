#include "ast/astnode.hh"
#include "astnode.hh"

namespace lona {

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
        default:
            throw std::runtime_error("Invalid type for AstConst");
    }
}

void
AstField::toJson(Json &root) {
    root["type"] = "field";
    root["name"] = this->name;
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
    root["op"] = symbolToStr(this->op);
    root["left"] = Json::object();
    this->left->toJson(root["left"]);
    root["right"] = Json::object();
    this->right->toJson(root["right"]);
}

void
AstUnaryOper::toJson(Json &root) {
    root["type"] = "UnaryOperator";
    root["op"] = symbolToStr(this->op);
    root["expr"] = Json::object();
    this->expr->toJson(root["expr"]);
}

void
AstVarDecl::toJson(Json &root) {
    root["type"] = "VarDecl";
    root["field"] = this->field;
    if (typeHelper) {
        root["typestr"] = typeHelper->toString();
    }
    if (right) {
        root["right"] = Json::object();
        this->right->toJson(root["right"]);
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
    root["name"] = this->name;
    if (this->retType) root["ret"] = this->retType->toString();
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
    root["value"] = Json::object();
    this->expr->toJson(root["value"]);
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

}  // namespace lona