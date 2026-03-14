#include "astnode.hh"
#include "../visitor.hh"
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace lona {

FuncTypeNode*
findFuncTypeNode(TypeNode *node) {
    if (node == nullptr) {
        return nullptr;
    }
    if (auto *func = dynamic_cast<FuncTypeNode *>(node)) {
        return func;
    }
    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        return findFuncTypeNode(pointer->base);
    }
    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        return findFuncTypeNode(array->base);
    }
    return nullptr;
}

TypeNode*
createPointerOrArrayTypeNode(TypeNode *head, std::vector<AstNode *> *suffix) {
    if (suffix == nullptr || suffix->empty()) {
        return head;
    }

    TypeNode *node = head;
    for (auto *it : *suffix) {
        if (it == nullptr) {
            node = new PointerTypeNode(node);
            continue;
        }
        if (it == reinterpret_cast<AstNode *>(1)) {
            node = new ArrayTypeNode(node);
            continue;
        }
        node = new ArrayTypeNode(node, std::vector<AstNode *>{it});
    }
    return node;
}

Object *
AstNode::accept(AstVisitor &) {
    throw std::runtime_error("Cannot visit abstract AstNode directly");
}

#define DEF_ACCEPT(classname)                \
    Object *classname::accept(AstVisitor &visitor) { \
        return visitor.visit(this);          \
    }

DEF_ACCEPT(AstProgram)
DEF_ACCEPT(AstStatList)
DEF_ACCEPT(AstConst)
DEF_ACCEPT(AstField)
DEF_ACCEPT(AstAssign)
DEF_ACCEPT(AstBinOper)
DEF_ACCEPT(AstUnaryOper)
DEF_ACCEPT(AstStructDecl)
DEF_ACCEPT(AstVarDecl)
DEF_ACCEPT(AstVarDef)
DEF_ACCEPT(AstFuncDecl)
DEF_ACCEPT(AstRet)
DEF_ACCEPT(AstIf)
DEF_ACCEPT(AstFor)
DEF_ACCEPT(AstFieldCall)
DEF_ACCEPT(AstSelector)

AstProgram::AstProgram(AstNode *body)
    : AstNode(body ? body->loc : location()), body(body->as<AstStatList>()) {
    assert(body->is<AstStatList>());
}

AstConst::AstConst(AstToken &token) : AstNode(token.loc) {
    switch (token.type) {
        case TokenType::ConstInt32:
            this->vtype = Type::INT32;
            this->buf = (char *)new int32_t(token.text.toI32());
            break;
        case TokenType::ConstFP32:
            this->vtype = Type::FP32;
            this->buf = (char *)new float(token.text.toF32());
            break;
        case TokenType::ConstStr:
            this->vtype = Type::STRING;
            this->buf = new char[token.text.size() + 1];
            std::strcpy(this->buf, token.text.tochara());
            break;
        case TokenType::ConstBool:
            this->vtype = Type::BOOL;
            this->buf = (char *)new bool(std::strcmp(token.text.tochara(), "true") == 0);
            break;
        default:
            throw std::runtime_error("Invalid token type for AstConst");
    }
}

AstField::AstField(AstToken &token) : AstNode(token.loc), name(token.text) {
    assert(token.type == TokenType::Field);
}

AstAssign::AstAssign(AstNode *left, AstNode *right)
    : AstNode(left ? left->loc : (right ? right->loc : location())),
      left(left), right(right) {}

AstBinOper::AstBinOper(AstNode *left, token_type op, AstNode *right)
    : AstNode(left ? left->loc : (right ? right->loc : location())),
      left(left), op(op), right(right) {}

AstUnaryOper::AstUnaryOper(token_type op, AstNode *expr)
    : AstNode(expr ? expr->loc : location()), op(op), expr(expr) {}

AstVarDecl::AstVarDecl(AstToken &field, TypeNode *typeNode, AstNode *right)
    : AstNode(field.loc),
      field(field.text),
      typeNode(typeNode),
      right(right) {}

void
AstStatList::push(AstNode *node) {
    this->body.push_back(node);
}

AstStatList::AstStatList(AstNode *node)
    : AstNode(node ? node->loc : location()) {
    this->body.push_back(node);
}

AstFuncDecl::AstFuncDecl(AstToken &name, AstNode *body,
                         std::vector<AstNode *> *args, TypeNode *retType)
    : AstNode(name.loc), name(name.text), body(body), args(args), retType(retType) {}

AstRet::AstRet(const location &loc, AstNode *expr) : AstNode(loc), expr(expr) {}

AstIf::AstIf(AstNode *condition, AstNode *then, AstNode *els)
    : AstNode(condition ? condition->loc : location()),
      condition(condition), then(then), els(els) {}

AstFor::AstFor(AstNode *expr, AstNode *body)
    : AstNode(expr ? expr->loc : location()), expr(expr), body(body) {
    body->setNextNode(this);
}

AstFieldCall::AstFieldCall(AstNode *value, std::vector<AstNode *> *args)
    : AstNode(value ? value->loc : location()), value(value), args(args) {}

}  // namespace lona
