#pragma once

#include <cassert>
#include <cstdint>
#include <list>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>
#include <vector>

#include "../ast/token.hh"
#include "location.hh"
#include "lona/obj/value.hh"

using Json = nlohmann::ordered_json;

namespace lona {
class CFGChecker;
class AstNode;
class AstIf;
class AstRet;
class AstVisitor;
class Object;
class Scope;
class Function;

class TypeClass;
class TypeManager;

using token_type = int;

const int pointerType_pointer = 1;
const int pointerType_autoArray = 2;
const int pointerType_fixedArray = 3;

class TypeNode {
    TypeNode* head_node;
protected:
    TypeClass* type_hold = nullptr;
public:
    void setHead(TypeNode* head) { head_node = head; }
    TypeNode* getHead() { return head_node;}

    virtual TypeClass* accept(TypeManager* typeMgr) = 0;
};

// var a type
class NormTypeNode : public TypeNode {
    std::string name;
public:
    NormTypeNode(std::string name): name(name) {}

    TypeClass* accept(TypeManager* typeMgr) override;
};

class PointerTypeNode : public TypeNode {
    int level = 0;
public:
    PointerTypeNode(int level): level(level) {}

    void incLevel() { level++; }

    TypeClass* accept(TypeManager* typeMgr) override;
};

class ArrayTypeNode : public TypeNode {
    std::vector<AstNode*> dim;
public:
    ArrayTypeNode(std::vector<AstNode*>& dim): dim(std::move(dim)) {}

    TypeClass* accept(TypeManager* typeMgr) override;
};

// var a ()
class FuncTypeNode : TypeNode {
    std::vector<TypeNode*> args;
    TypeNode* ret = nullptr;
public:
    FuncTypeNode() {}
    FuncTypeNode(std::vector<TypeNode*> args): args(args) {}
    void setRet(TypeNode* ret) { this->ret = ret; }

    TypeClass* accept(TypeManager* typeMgr) override;
};

extern TypeNode* createPointerOrArrayTypeNode(TypeNode* head, std::vector<AstNode*>* suffix);

class AstNode {
protected:
    AstNode *cfg_next = nullptr;

public:
    location const loc;
    AstNode() {}
    AstNode(const location &loc) : loc(loc) {}

    virtual void setNextNode(AstNode *node) {
        cfg_next = node->getValidCFGNode();
    }
    virtual AstNode *getValidCFGNode() { return this; }
    virtual Object *accept(AstVisitor &visitor) = 0;
    virtual void toJson(Json &root) {};
    virtual void toCFG(CFGChecker &checker);

    bool isControlNode() { return is<AstIf>() || is<AstRet>(); }

    template<typename T>
    bool is() const {
        return dynamic_cast<const T *>(this) != nullptr;
    }

    template<typename T>
    T *as() {
        return dynamic_cast<T *>(this);
    }
};

class AstStatList;

class AstProgram : public AstNode {
public:
    AstStatList *const body;
    AstProgram(AstNode *body);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstConst : public AstNode {
public:
    enum class Type { INT32, FP32, STRING };

private:
    // the type of the constant
    Type vtype;
    char *buf = nullptr;  // never delete
public:
    Type getType() const { return vtype; }
    template<typename T = char>
    T *getBuf() const {
        return (T *)buf;
    }

    AstConst(AstToken &token);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstField : public AstNode {
public:
    std::string const name;
    AstField(AstToken &token);
    // AstField(std::string &token);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstAssign : public AstNode {
public:
    AstNode *const left;
    AstNode *const right;
    AstAssign(AstNode *left, AstNode *right);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstBinOper : public AstNode {
public:
    AstNode *const left;
    token_type const op;
    AstNode *const right;
    AstBinOper(AstNode *left, token_type op, AstNode *right);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstUnaryOper : public AstNode {
public:
    token_type const op;
    AstNode *const expr;

    AstUnaryOper(token_type op, AstNode *expr);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstStructDecl : public AstNode {
public:
    std::string const name;
    AstNode *const body;

    AstStructDecl(AstToken &field, AstNode *body)
        : name(field.text), body(body) {}
    Object *accept(AstVisitor &visitor) override;
};

class AstVarDecl : public AstNode {
public:
    std::string const field;
    TypeNode *const typeNode;
    AstNode *const right;

    AstVarDecl(AstToken &field, TypeNode *typeNode,
               AstNode *right = nullptr);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstVarDef : public AstNode {
    std::string const field;
    TypeNode *const typeNode;
    AstNode *const initVal;
public:
    AstVarDef(AstVarDecl *vardecl, AstNode *initVal = nullptr)
        : field(vardecl->field), typeNode(vardecl->typeNode), initVal(initVal) {}

    AstVarDef(AstToken &field,
               AstNode *initVal = nullptr)
        : field(field.text), typeNode(nullptr), initVal(initVal) {}


    auto& getName() const { return field; }
    TypeNode *getTypeNode() const { return typeNode; }
    AstNode *getInitVal() const { return initVal; }

    bool withInitVal() const { return initVal != nullptr; }
};

class AstStatList : public AstNode {
public:
    std::list<AstNode *> body;
    bool isEmpty() const { return body.empty(); }
    void push(AstNode *node);
    std::list<AstNode *> &getBody() { return body; }
    void setNextNode(AstNode *node) override {
        if (!body.empty()) {
            body.back()->setNextNode(node->getValidCFGNode());
        }
    }
    AstNode *getValidCFGNode() override {
        return body.empty() ? nullptr : body.front();
    }

    AstStatList() {}
    AstStatList(AstNode *node);
    void toJson(Json &root) override;
    void toCFG(CFGChecker &checker) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstFuncDecl : public AstNode {
public:
    std::string const name;
    std::vector<AstNode *> *const args = nullptr;
    AstNode *const body;
    TypeNode *const retType;
    bool hasArgs() const { return args != nullptr; }

    AstFuncDecl(AstToken &name, AstNode *body,
                std::vector<AstNode *> *args = nullptr,
                TypeNode *retType = nullptr);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstRet : public AstNode {
public:
    AstNode *const expr = nullptr;
    void setNextNode(AstNode *node) override {
        // do nothing
    }

    AstRet(const location &loc, AstNode *expr);
    void toJson(Json &root) override;
    void toCFG(CFGChecker &checker) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstIf : public AstNode {
public:
    AstNode *const condition;
    AstNode *const then;
    AstNode *const els = nullptr;
    bool hasElse() const { return els != nullptr; }
    void setNextNode(AstNode *node) override {
        then->setNextNode(node->getValidCFGNode());
        if (els)
            els->setNextNode(node->getValidCFGNode());
        else
            cfg_next = node->getValidCFGNode();
    }

    AstIf(AstNode *condition, AstNode *then, AstNode *els = nullptr);

    void toJson(Json &root) override;
    void toCFG(CFGChecker &checker) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstFor : public AstNode {
public:
    AstNode *const expr;
    AstNode *const body;

    AstFor(AstNode *expr, AstNode *body);
    void toJson(Json &root) override;
    void toCFG(CFGChecker &checker) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstFieldCall : public AstNode {
public:
    AstNode *const value;
    std::vector<AstNode *> *const args = nullptr;

    AstFieldCall(AstNode *value, std::vector<AstNode *> *args = nullptr);

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstSelector : public AstNode {
public:
    AstNode *const parent;
    AstToken *const field;
    AstSelector(AstNode *parent, AstToken *field)
        : parent(parent), field(field) {}

    Object *accept(AstVisitor &visitor) override;
};

}  // namespace lona