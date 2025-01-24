#include "ast/astnode.hh"
#include "astnode.hh"
#include "visitor/base.hh"

namespace lona {

#define DEF_ACCEPT(classname)                              \
    BaseVariable *classname::accept(AstVisitor &visitor) { \
        return visitor.visit(this);                        \
    }

DEF_ACCEPT(AstNode)
DEF_ACCEPT(AstStatList)
DEF_ACCEPT(AstProgram)
DEF_ACCEPT(AstConst)
DEF_ACCEPT(AstField)
DEF_ACCEPT(AstAssign)
DEF_ACCEPT(AstBinOper)
DEF_ACCEPT(AstUnaryOper)
DEF_ACCEPT(AstVarDecl)
DEF_ACCEPT(AstFuncDecl)
DEF_ACCEPT(AstRet)
DEF_ACCEPT(AstIf)
DEF_ACCEPT(AstFieldCall)


std::string
lona::TypeHelper::toString() {

    std::string full_type = typeName.front();
    for (size_t i = 1; i < typeName.size(); i++) {
        if (typeName[i].front() == '!') {
            // function pointer
            full_type += "!(";
            if (func_args)
                for (auto &it : *func_args) {
                    full_type += it->toString();
                    full_type += ",";
                }
            full_type += ")";
        } else {
            full_type += ".";
            full_type += typeName[i];
        }
    }
    if (levels)
        for (auto it : *levels) {
            if (it.first == pointerType_pointer) {
                full_type += "*";
            } else if (it.first == pointerType_autoArray) {
                full_type += "[]";
            } else if (it.first == pointerType_fixedArray) {
                full_type += "[";
                full_type += std::string(it.second->is<AstConst>()
                                 ? it.second->as<AstConst>().getBuf()
                                 : "x") + "<";
                full_type += "]";
            }
        }

    return full_type;
}

AstProgram::AstProgram(AstNode *body) : body(&body->as<AstStatList>()) {
    assert(body->is<AstStatList>());
}

AstConst::AstConst(AstToken &token) {
    const int a = 1;
    switch (token.type) {
        case TokenType::ConstInt32:
            this->vtype = Type::INT32;
            this->buf = (char *)new int32_t(std::stoul(token.text));
            break;
        case TokenType::ConstFP32:
            this->vtype = Type::FP32;
            this->buf = (char *)new float(std::stod(token.text));
            break;
        case TokenType::ConstStr:
            this->vtype = Type::STRING;
            this->buf = new char[token.text.size() + 1];
            strcpy(this->buf, token.text.c_str());
            break;
        default:
            throw std::runtime_error("Invalid token type for AstConst");
    }
}

AstField::AstField(AstToken &token) : name(token.text) {
    assert(token.type == TokenType::Field);
}

AstField::AstField(std::string &token) : name(token) {}

AstAssign::AstAssign(AstNode *left, AstNode *right)
    : left(left), right(right) {}

AstBinOper::AstBinOper(AstNode *left, token_type op, AstNode *right)
    : left(left), op(op), right(right) {}

AstUnaryOper::AstUnaryOper(token_type op, AstNode *expr) : op(op), expr(expr) {}

AstVarDecl::AstVarDecl(AstToken &field, TypeHelper *typeHelper, AstNode *right)
    : field(field.text), typeHelper(typeHelper), right(right) {}

void
AstStatList::push(AstNode *node) {
    this->body.push_back(node);
}

AstStatList::AstStatList(AstNode *node) { this->body.push_back(node); }

AstFuncDecl::AstFuncDecl(AstToken &name, AstNode *body,
                         std::list<AstTypeDecl> *args) {
    assert(body->is<AstStatList>());
    this->name = name.text;
    this->body = body;
    this->argdecl = args;
}

AstRet::AstRet(AstNode *expr) { this->expr = expr; }

AstIf::AstIf(AstNode *condition, AstNode *then, AstNode *els)
    : condition(condition), then(then), els(els) {}

AstFieldCall::AstFieldCall(AstToken &field, std::list<AstNode *> *args) {
    assert(field.type == TokenType::Field);
    this->name = field.text;
    this->args = args;
}

}