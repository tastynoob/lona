#include "astnode.hh"
#include "../visitor.hh"
#include "../type/scope.hh"
#include <cassert>
#include <cstddef>

namespace lona {


TypeClass* 
NormTypeNode::accept(TypeManager* typeMgr) {
    if (type_hold) {
        return type_hold;
    }

    type_hold = typeMgr->getType(this->name);
    return type_hold;
}

TypeClass* 
PointerTypeNode::accept(TypeManager* typeMgr) {
    if (!type_hold) {
        return type_hold;
    }

    auto head_type = getHead()->accept(typeMgr);
    for (int i=0;i<level;i++) {
        head_type = typeMgr->createPointerType(head_type);
    }

    return head_type;
}

TypeClass* 
ArrayTypeNode::accept(TypeManager* typeMgr) {
    return nullptr;
}

TypeClass* 
FuncTypeNode::accept(TypeManager* typeMgr) {
    return nullptr;
}

TypeNode* createPointerOrArrayTypeNode(TypeNode* head, std::vector<AstNode*>* suffix)
{
    TypeNode* node = nullptr;
    AstNode* prev_val = (AstNode*)-1llu;
    for (auto it : *suffix) {
        if (prev_val == 0 && it == 0) {
            // multi pointer
            static_cast<PointerTypeNode*>(node)->incLevel();
            continue;
        } else {
            assert(false);
        }

        TypeNode* new_node = nullptr;
        if (it == 0) {
            // pointer
            new_node = new PointerTypeNode(1);
        } else if (it == (AstNode*)1) {
            // auto array size
            assert(false);
        } else {
            // fixed array size
            assert(false);
        }

        if (node == nullptr) {
            new_node->setHead(head);
        }
        if (node != nullptr) {
            new_node->setHead(node);
        }

        node = new_node;
        prev_val = it;
    }
    return node;
}


#define DEF_ACCEPT(classname)                        \
    Object *classname::accept(AstVisitor &visitor) { \
        return visitor.visit(this);                  \
    }

DEF_ACCEPT(AstNode)
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

AstProgram::AstProgram(AstNode *body) : body(body->as<AstStatList>()) {
    assert(body->is<AstStatList>());
}

AstConst::AstConst(AstToken &token) : AstNode(token.loc) {
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

AstField::AstField(AstToken &token) : AstNode(token.loc), name(token.text) {
    assert(token.type == TokenType::Field);
}

AstAssign::AstAssign(AstNode *left, AstNode *right)
    : left(left), right(right) {}

AstBinOper::AstBinOper(AstNode *left, token_type op, AstNode *right)
    : left(left), op(op), right(right) {}

AstUnaryOper::AstUnaryOper(token_type op, AstNode *expr) : op(op), expr(expr) {}

AstVarDecl::AstVarDecl(AstToken &field, TypeNode *typeNode, AstNode *right)
    : AstNode(field.loc),
      field(field.text),
      typeNode(typeNode),
      right(right) {}

void
AstStatList::push(AstNode *node) {
    this->body.push_back(node);
}

AstStatList::AstStatList(AstNode *node) { this->body.push_back(node); }

AstFuncDecl::AstFuncDecl(AstToken &name, AstNode *body,
                         std::vector<AstNode *> *args, TypeNode *retType)
    : name(name.text), body(body), args(args), retType(retType) {}

AstRet::AstRet(const location &loc, AstNode *expr) : AstNode(loc), expr(expr) {}

AstIf::AstIf(AstNode *condition, AstNode *then, AstNode *els)
    : condition(condition), then(then), els(els) {}

AstFor::AstFor(AstNode *expr, AstNode *body) : expr(expr), body(body) {
    body->setNextNode(this);
}

AstFieldCall::AstFieldCall(AstNode *value, std::vector<AstNode *> *args)
    : value(value), args(args) {}

}