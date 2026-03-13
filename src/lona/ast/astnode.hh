#pragma once

#include <cassert>
#include <cstdint>
#include <list>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>
#include <vector>

#include "../err/err.hh"
#include "../ast/token.hh"
#include "location.hh"
#include "../sym/object.hh"
#include "../util/string.hh"

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
class TypeTable;

using token_type = int;

const int pointerType_pointer = 1;
const int pointerType_autoArray = 2;
const int pointerType_fixedArray = 3;

struct TypeNode {
    virtual ~TypeNode() = default;
};

struct BaseTypeNode : public TypeNode {
    string const name;
    BaseTypeNode(string name) : name(name) {}
};

struct PointerTypeNode : public TypeNode {
    TypeNode *base;
    uint32_t dim;

    PointerTypeNode(TypeNode *base, uint32_t dim = 1) : base(base), dim(dim) {}
};

struct ArrayTypeNode : public TypeNode {
    TypeNode *base;
    std::vector<AstNode*> dim;

    ArrayTypeNode(TypeNode *base, std::vector<AstNode*> dim = {})
        : base(base), dim(std::move(dim)) {}
};

struct FuncTypeNode : public TypeNode {
    std::vector<TypeNode*> args;
    TypeNode* ret = nullptr;

    FuncTypeNode(std::vector<TypeNode*> args = {}, TypeNode* ret = nullptr)
        : args(std::move(args)), ret(ret) {}
};

extern FuncTypeNode* findFuncTypeNode(TypeNode* node);
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
    virtual bool hasTerminator() { return false; }
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
    string const name;
    AstField(AstToken &token);
    // AstField(string &token);
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
    string const name;
    AstNode *const body;

    AstStructDecl(AstToken &field, AstNode *body)
        : name(field.text), body(body) {}
    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstVarDecl : public AstNode {
public:
    string const field;
    TypeNode *const typeNode;
    AstNode *const right;

    AstVarDecl(AstToken &field, TypeNode *typeNode,
               AstNode *right = nullptr);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstVarDef : public AstNode {
    string const field;
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

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
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

    bool hasTerminator() override {
        for (auto it = body.rbegin(); it != body.rend(); ++it) {
            if ((*it)->hasTerminator()) {
                return true;
            }
        }
        return false;
    }

    AstStatList() {}
    AstStatList(AstNode *node);
    void toJson(Json &root) override;
    void toCFG(CFGChecker &checker) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstFuncDecl : public AstNode {
public:
    string const name;
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

    bool hasTerminator() override {
        return true;
    }

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

    bool hasTerminator() override {
        if (els == nullptr) {
            return false;
        }
        return then->hasTerminator() && els->hasTerminator();
    }

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

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

}  // namespace lona