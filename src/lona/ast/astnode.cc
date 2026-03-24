#include "astnode.hh"
#include "../visitor.hh"
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace lona {

FuncPtrTypeNode*
findFuncPtrTypeNode(TypeNode *node) {
    if (node == nullptr) {
        return nullptr;
    }
    if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
        return findFuncPtrTypeNode(param->type);
    }
    if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
        return func;
    }
    if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
        return findFuncPtrTypeNode(qualified->base);
    }
    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        return findFuncPtrTypeNode(pointer->base);
    }
    if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
        return findFuncPtrTypeNode(indexable->base);
    }
    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        return findFuncPtrTypeNode(array->base);
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
            node = new PointerTypeNode(node, 1, node ? node->loc : location());
            continue;
        }
        if (it == reinterpret_cast<AstNode *>(1)) {
            node = new ArrayTypeNode(node, {}, node ? node->loc : location());
            continue;
        }
        node = new ArrayTypeNode(node, std::vector<AstNode *>{it},
                                 node ? node->loc : location());
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
DEF_ACCEPT(AstTagNode)
DEF_ACCEPT(AstStatList)
DEF_ACCEPT(AstConst)
DEF_ACCEPT(AstField)
DEF_ACCEPT(AstFuncRef)
DEF_ACCEPT(AstAssign)
DEF_ACCEPT(AstBinOper)
DEF_ACCEPT(AstUnaryOper)
DEF_ACCEPT(AstRefExpr)
DEF_ACCEPT(AstTupleLiteral)
DEF_ACCEPT(AstBraceInitItem)
DEF_ACCEPT(AstBraceInit)
DEF_ACCEPT(AstNamedCallArg)
DEF_ACCEPT(AstStructDecl)
DEF_ACCEPT(AstImport)
DEF_ACCEPT(AstVarDecl)
DEF_ACCEPT(AstVarDef)
DEF_ACCEPT(AstFuncDecl)
DEF_ACCEPT(AstRet)
DEF_ACCEPT(AstBreak)
DEF_ACCEPT(AstContinue)
DEF_ACCEPT(AstIf)
DEF_ACCEPT(AstFor)
DEF_ACCEPT(AstCastExpr)
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
        case TokenType::ConstFP64:
            this->vtype = Type::FP64;
            this->buf = (char *)new double(std::stod(token.text.tochara()));
            break;
        case TokenType::ConstStr:
            this->vtype = Type::STRING;
            this->buf = new string(token.text);
            break;
        case TokenType::ConstChar:
            this->vtype = Type::CHAR;
            this->buf = new string(token.text);
            break;
        case TokenType::ConstBool:
            this->vtype = Type::BOOL;
            this->buf = (char *)new bool(std::strcmp(token.text.tochara(), "true") == 0);
            break;
        case TokenType::ConstNull:
            this->vtype = Type::NULLPTR;
            this->buf = nullptr;
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

AstVarDecl::AstVarDecl(BindingKind bindingKind, AstToken &field,
                       TypeNode *typeNode, AstNode *right)
    : AstNode(field.loc),
      bindingKind(bindingKind),
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
                         std::vector<AstNode *> *args, TypeNode *retType,
                         AbiKind abiKind)
    : AstNode(name.loc),
      name(name.text),
      args(args),
      body(body),
      retType(retType),
      abiKind(abiKind) {}

AstRet::AstRet(const location &loc, AstNode *expr) : AstNode(loc), expr(expr) {}

AstIf::AstIf(AstNode *condition, AstNode *then, AstNode *els)
    : AstNode(condition ? condition->loc : location()),
      condition(condition), then(then), els(els) {}

AstFor::AstFor(AstNode *expr, AstNode *body, AstNode *els)
    : AstNode(expr ? expr->loc : location()), expr(expr), body(body), els(els) {}

AstFieldCall::AstFieldCall(AstNode *value, std::vector<AstNode *> *args)
    : AstNode(value ? value->loc : location()), value(value), args(args) {}

}  // namespace lona
