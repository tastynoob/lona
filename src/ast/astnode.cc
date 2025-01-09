#include "ast/astnode.hh"
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
DEF_ACCEPT(AstVarInitAssign)
DEF_ACCEPT(AstFuncDecl)
DEF_ACCEPT(AstRet)
DEF_ACCEPT(AstIf)
DEF_ACCEPT(AstFieldCall)

AstProgram::AstProgram(AstNode *body) : body(&body->as<AstStatList>()) {
    assert(body->is<AstStatList>());
}

AstConst::AstConst(AstToken &token) {
    const int a = 1;
    switch (token.getType()) {
        case TokenType::ConstInt32:
            this->vtype = Type::INT32;
            this->buf = (char *)new int32_t(std::stoul(token.getText()));
            break;
        case TokenType::ConstFP32:
            this->vtype = Type::FP32;
            this->buf = (char *)new float(std::stod(token.getText()));
            break;
        case TokenType::ConstStr:
            this->vtype = Type::STRING;
            this->buf = new char[token.getText().size() + 1];
            strcpy(this->buf, token.getText().c_str());
            break;
        default:
            throw std::runtime_error("Invalid token type for AstConst");
    }
}

AstField::AstField(AstToken &token) : name(token.getText()) {
    assert(token.getType() == TokenType::Field);
}

AstAssign::AstAssign(AstNode *left, AstNode *right)
    : left(left), right(right) {}

AstBinOper::AstBinOper(AstNode *left, AstToken &op, AstNode *right)
    : left(left), op(strToSymbol(op.getText().c_str())), right(right) {}

AstUnaryOper::AstUnaryOper(AstToken &op, AstNode *expr)
    : op(strToSymbol(op.getText().c_str())), expr(expr) {}

AstUnaryOper::AstUnaryOper(SymbolTable op, AstNode *expr)
    : op(op), expr(expr) {}

AstVarDecl::AstVarDecl(AstToken &field, TypeHelper* typeHelper)
    : field(field.getText()), typeHelper(typeHelper) {}

AstVarInitAssign::AstVarInitAssign(AstNode* left, AstNode* right)
    : left(left), right(right), isAutoInfer(left->is<AstField>()) {}

void
AstStatList::push(AstNode *node) {
    this->body.push_back(node);
}

AstStatList::AstStatList(AstNode *node) { this->body.push_back(node); }

AstFuncDecl::AstFuncDecl(AstToken &name, AstNode *body,
                         std::list<AstTypeDecl> *args) {
    assert(body->is<AstStatList>());
    this->name = name.getText();
    this->body = body;
    this->argdecl = args;
}

AstRet::AstRet(AstNode *expr) { this->expr = expr; }

AstIf::AstIf(AstNode *condition, AstNode *then, AstNode *els)
    : condition(condition), then(then), els(els) {}

AstFieldCall::AstFieldCall(AstToken &field, std::list<AstNode *> *args) {
    assert(field.getType() == TokenType::Field);
    this->name = field.getText();
    this->args = args;
}

}  // namespace lona