#include "astnode.hh"
#include "type_node_string.hh"

namespace lona {
namespace {

std::string
escapeByteStringForJson(const string &value) {
    static constexpr char kHexDigits[] = "0123456789ABCDEF";

    std::string escaped;
    escaped.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(value[i]);
        switch (byte) {
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\0':
            escaped += "\\0";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\"':
            escaped += "\\\"";
            break;
        default:
            if (byte >= 0x20 && byte <= 0x7E) {
                escaped.push_back(static_cast<char>(byte));
            } else {
                escaped += "\\x";
                escaped.push_back(kHexDigits[(byte >> 4) & 0x0F]);
                escaped.push_back(kHexDigits[byte & 0x0F]);
            }
            break;
        }
    }

    return escaped;
}

}  // namespace

void
AstTag::toJson(Json &root) const {
    root["name"] = name.text.tochara();
    root["args"] = Json::array();
    if (!args) {
        return;
    }
    for (auto *arg : *args) {
        if (!arg) {
            continue;
        }
        Json value = Json::object();
        value["type"] = tokenTypeToStr(arg->type);
        value["value"] = arg->text.tochara();
        root["args"].push_back(value);
    }
}

void
AstProgram::toJson(Json &root) {
    root["type"] = "Program";
    root["body"] = Json::object();
    this->body->toJson(root["body"]);
}

void
AstTagNode::toJson(Json &root) {
    root["type"] = "Tag";
    root["tags"] = Json::array();
    if (tags) {
        for (auto *tag : *tags) {
            Json item = Json::object();
            if (tag) {
                tag->toJson(item);
            }
            root["tags"].push_back(item);
        }
    }
}

void
AstConst::toJson(Json &root) {
    root["type"] = "const";
    switch (this->vtype) {
        case Type::I8:
            root["value"] = isUnaryMinusOnlySignedMinLiteral()
                ? getDeferredSignedMinMagnitude()
                : static_cast<int>(*getBuf<std::int8_t>());
            break;
        case Type::U8:
            root["value"] = static_cast<unsigned>(*getBuf<std::uint8_t>());
            break;
        case Type::I16:
            root["value"] = isUnaryMinusOnlySignedMinLiteral()
                ? getDeferredSignedMinMagnitude()
                : *getBuf<std::int16_t>();
            break;
        case Type::U16:
            root["value"] = *getBuf<std::uint16_t>();
            break;
        case Type::I32:
            root["value"] = isUnaryMinusOnlySignedMinLiteral()
                ? getDeferredSignedMinMagnitude()
                : *getBuf<std::int32_t>();
            break;
        case Type::U32:
            root["value"] = *getBuf<std::uint32_t>();
            break;
        case Type::I64:
            root["value"] = isUnaryMinusOnlySignedMinLiteral()
                ? getDeferredSignedMinMagnitude()
                : static_cast<long long>(*getBuf<std::int64_t>());
            break;
        case Type::U64:
            root["value"] = *getBuf<std::uint64_t>();
            break;
        case Type::USIZE:
            root["value"] = *getBuf<std::uint64_t>();
            break;
        case Type::F32:
            root["value"] = *getBuf<float>();
            break;
        case Type::F64:
            root["value"] = *getBuf<double>();
            break;
        case Type::STRING:
            root["value"] = escapeByteStringForJson(*this->getBuf<string>());
            break;
        case Type::CHAR:
            root["value"] = escapeByteStringForJson(*this->getBuf<string>());
            break;
        case Type::BOOL:
            root["value"] = *getBuf<bool>();
            break;
        case Type::NULLPTR:
            root["value"] = nullptr;
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
            root["args"].push_back(describeTypeNode(argType));
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
AstRefExpr::toJson(Json &root) {
    root["type"] = "RefExpr";
    root["expr"] = Json::object();
    if (expr) {
        expr->toJson(root["expr"]);
    }
}

void
AstTupleLiteral::toJson(Json &root) {
    root["type"] = "TupleLiteral";
    root["items"] = Json::array();
    if (!items) {
        return;
    }
    for (auto *item : *items) {
        Json child = Json::object();
        item->toJson(child);
        root["items"].push_back(child);
    }
}

void
AstBraceInitItem::toJson(Json &root) {
    root["type"] = "BraceInitItem";
    root["kind"] = "positional";
    root["value"] = Json::object();
    if (value) {
        value->toJson(root["value"]);
    }
}

void
AstBraceInit::toJson(Json &root) {
    root["type"] = "BraceInit";
    root["items"] = Json::array();
    if (!items) {
        return;
    }
    for (auto *item : *items) {
        Json child = Json::object();
        item->toJson(child);
        root["items"].push_back(child);
    }
}

void
AstNamedCallArg::toJson(Json &root) {
    root["type"] = "NamedCallArg";
    root["name"] = name.tochara();
    root["value"] = Json::object();
    if (value) {
        value->toJson(root["value"]);
    }
}

void
AstStructDecl::toJson(Json &root) {
    root["type"] = "StructDecl";
    root["name"] = this->name.tochara();
    root["declKind"] = structDeclKindKeyword(this->declKind);
    if (this->body) {
        root["body"] = Json::object();
        this->body->toJson(root["body"]);
    } else {
        root["body"] = nullptr;
    }
}

void
AstImport::toJson(Json &root) {
    root["type"] = "Import";
    root["path"] = path;
}

void
AstVarDecl::toJson(Json &root) {
    root["type"] = "VarDecl";
    root["bindingKind"] = bindingKindKeyword(this->bindingKind);
    root["accessKind"] = accessKindKeyword(this->accessKind);
    root["field"] = this->field.tochara();
    if (typeNode) {
        root["declaredType"] = describeTypeNode(typeNode);
    }
    if (right) {
        root["right"] = Json::object();
        this->right->toJson(root["right"]);
    }
}

void
AstVarDef::toJson(Json &root) {
    root["type"] = "VarDef";
    root["bindingKind"] = bindingKindKeyword(this->bindingKind);
    root["constBinding"] = this->isConstBinding();
    root["field"] = this->field.tochara();
    if (this->typeNode != nullptr) {
        root["declaredType"] = describeTypeNode(this->typeNode);
    }
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
    root["abiKind"] = abiKindKeyword(this->abiKind);
    root["receiverAccess"] = accessKindKeyword(this->receiverAccess);
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
    if (this->body) {
        root["body"] = Json::object();
        this->body->toJson(root["body"]);
    } else {
        root["body"] = nullptr;
    }
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
AstBreak::toJson(Json &root) {
    root["type"] = "Break";
}

void
AstContinue::toJson(Json &root) {
    root["type"] = "Continue";
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
    if (this->els != nullptr) {
        root["else"] = Json::object();
        this->els->toJson(root["else"]);
    }
}

void
AstCastExpr::toJson(Json &root) {
    root["type"] = "CastExpr";
    root["targetType"] = describeTypeNode(this->targetType);
    root["value"] = Json::object();
    this->value->toJson(root["value"]);
}

void
AstSizeofExpr::toJson(Json &root) {
    root["type"] = "SizeofExpr";
    if (this->targetType) {
        root["targetType"] = describeTypeNode(this->targetType);
    } else {
        root["targetType"] = "none";
    }
    if (this->value) {
        root["value"] = Json::object();
        this->value->toJson(root["value"]);
    } else {
        root["value"] = "none";
    }
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
AstDotLike::toJson(Json &root) {
    root["type"] = "DotLike";
    root["parent"] = Json::object();
    this->parent->toJson(root["parent"]);
    root["field"] = this->field->text.tochara();
}

}  // namespace lona
