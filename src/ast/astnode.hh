#pragma once

#include <cassert>
#include <cstdint>
#include <list>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>

#include "ast/token.hh"
#include "location.hh"

using Json = nlohmann::ordered_json;

namespace lona {
class CFGChecker;
class AstNode;
class AstIf;
class AstRet;
class AstVisitor;
class Object;
class Scope;
class Method;
using token_type = int;

const int pointerType_pointer = 1;
const int pointerType_autoArray = 2;
const int pointerType_fixedArray = 3;
struct TypeHelper {
    std::vector<std::string> typeName;
    std::vector<TypeHelper *> *func_args = nullptr;
    TypeHelper *func_retType = nullptr;
    std::vector<std::pair<int, AstNode *>> *levels = nullptr;
    TypeHelper(std::string typeName) { this->typeName.push_back(typeName); }
    std::string toString();
    bool isPointerOrArray() { return levels->size() > 0; }
};

class AstNode {
protected:
    AstNode *cfg_next = nullptr;

public:
    location const loc;
    AstNode() {}
    AstNode(location loc) : loc(loc) {}

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
    TypeHelper *const typeHelper;
    AstNode *const right;

    AstVarDecl(AstToken &field, TypeHelper *typeHelper,
               AstNode *right = nullptr);
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
    TypeHelper *const retType;
    std::string getName() { return name; }
    bool hasArgs() const { return args != nullptr; }

    AstFuncDecl(AstToken &name, AstNode *body,
                std::vector<AstNode *> *args = nullptr,
                TypeHelper *retType = nullptr);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstRet : public AstNode {
public:
    AstNode *const expr = nullptr;
    void setNextNode(AstNode *node) override {
        // do nothing
    }

    AstRet(AstNode *expr);
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