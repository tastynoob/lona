#include "ast/astnode.hh"
#include "astnode.hh"
#include "codegen/base.hh"
#include "type/scope.hh"

namespace lona {

#define DEF_ACCEPT(classname)                        \
    Object *classname::accept(AstVisitor &visitor) { \
        return visitor.visit(this);                  \
    }

DEF_ACCEPT(AstNode)
DEF_ACCEPT(AstStatList)
DEF_ACCEPT(AstProgram)
DEF_ACCEPT(AstConst)
DEF_ACCEPT(AstField)
DEF_ACCEPT(AstAssign)
DEF_ACCEPT(AstBinOper)
DEF_ACCEPT(AstUnaryOper)
DEF_ACCEPT(AstStructDecl)
DEF_ACCEPT(AstVarDecl)
DEF_ACCEPT(AstFuncDecl)
DEF_ACCEPT(AstRet)
DEF_ACCEPT(AstIf)
DEF_ACCEPT(AstFieldCall)
DEF_ACCEPT(AstSelector)

std::string
TypeHelper::toString() {

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
                full_type +=
                    std::string(it.second->is<AstConst>()
                                    ? it.second->as<AstConst>().getBuf()
                                    : "x") +
                    "<";
                full_type += "]";
            }
        }

    return full_type;
}

AstProgram::AstProgram(AstNode *body) : body(&body->as<AstStatList>()) {
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

AstVarDecl::AstVarDecl(AstToken &field, TypeHelper *typeHelper, AstNode *right)
    : AstNode(field.loc),
      field(field.text),
      typeHelper(typeHelper),
      right(right) {}

void
AstStatList::push(AstNode *node) {
    this->body.push_back(node);
}

AstStatList::AstStatList(AstNode *node) { this->body.push_back(node); }

Functional *
AstFuncDecl::createFunc(Scope &scope) {
    // create function type
    auto &builder = scope.getBuilder();
    std::vector<llvm::Type *> args;
    std::vector<TypeClass *> loargs;
    llvm::Type *retType = builder.getVoidTy();
    TypeClass *loretType = nullptr;
    if (this->args)
        for (auto it : *this->args) {
            auto decl = dynamic_cast<AstVarDecl *>(it);
            assert(decl);
            if (!decl->typeHelper) {
                throw "type of argument is not allowed auto infer";
            }
            auto type = scope.getType(decl->typeHelper);
            args.push_back(type->getllvmType());
            loargs.push_back(type);
        }
    if (this->retType) {
        loretType = scope.getType(this->retType);
        retType = loretType->getllvmType();
    }
    llvm::FunctionType *funcType =
        llvm::FunctionType::get(retType, args, false);

    std::string func_name = scope.getName() + '.' + this->name;
    // create function
    llvm::Function *func =
        llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                               func_name, scope.getModule());

    return new Functional(func,
                          new FuncType(funcType, std::move(loargs), loretType));
}

AstFuncDecl::AstFuncDecl(AstToken &name, AstNode *body,
                         std::vector<AstNode *> *args, TypeHelper *retType)
    : name(name.text), body(body), args(args), retType(retType) {}

AstRet::AstRet(AstNode *expr) { this->expr = expr; }

AstIf::AstIf(AstNode *condition, AstNode *then, AstNode *els)
    : condition(condition), then(then), els(els) {}

AstFieldCall::AstFieldCall(AstNode *value, std::vector<AstNode *> *args)
    : value(value), args(args) {}

}