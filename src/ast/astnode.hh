#pragma once

#include <cassert>
#include <cstdint>
#include <list>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>

#include "ast/token.hh"

using Json = nlohmann::ordered_json;

namespace lona {
class AstVisitor;
class BaseVariable;

const uint64_t pointerType_pointer = UINT64_MAX - 1;
const uint64_t pointerType_autoArray = UINT64_MAX - 2;
struct TypeHelper {
    std::string const typeName;
    std::vector<uint64_t> levels;
    TypeHelper* func_retType = nullptr;
    TypeHelper(std::string typeName) : typeName(std::move(typeName)) {}
    bool isPointerOrArray() { return levels.size() > 0; }
};

class AstNode {
    int row = -1, col = -1;

public:
    AstNode() {}
    AstNode(int row, int col) : row(row), col(col) {}

    virtual BaseVariable *accept(AstVisitor &visitor) = 0;
    virtual void toJson(Json &root) {};

    template<typename T>
    bool is() const {
        return dynamic_cast<const T *>(this) != nullptr;
    }

    template<typename T>
    T &as() {
        return dynamic_cast<T &>(*this);
    }
};

class AstStatList;
class AstProgram : public AstNode {
public:
    AstStatList *const body;
    AstProgram(AstNode *body);
    void toJson(Json &root) override;

    BaseVariable *accept(AstVisitor &visitor) override;
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

    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstField : public AstNode {
public:
    std::string const name;
    AstField(AstToken &token);
    void toJson(Json &root) override;

    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstAssign : public AstNode {
public:
    AstNode *const left;
    AstNode *const right;
    AstAssign(AstNode *left, AstNode *right);
    void toJson(Json &root) override;

    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstBinOper : public AstNode {
public:
    AstNode *const left;
    SymbolTable const op;
    AstNode *const right;
    AstBinOper(AstNode *left, AstToken &op, AstNode *right);
    void toJson(Json &root) override;

    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstUnaryOper : public AstNode {
public:
    SymbolTable const op;
    AstNode *const expr;

    AstUnaryOper(AstToken &op, AstNode *expr);
    AstUnaryOper(SymbolTable op, AstNode *expr);
    void toJson(Json &root) override;

    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstVarDecl : public AstNode {
public:
    std::string const field;
    TypeHelper* const typeHelper;

    AstVarDecl(AstToken &field, TypeHelper* typeHelper);
    void toJson(Json &root) override;

    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstVarInitAssign : public AstNode {
public:
    AstNode*const left;
    AstNode*const right;
    bool const isAutoInfer;
    AstVarInitAssign(AstNode* left, AstNode* right);
    void toJson(Json &root) override;

    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstStatList : public AstNode {
    std::list<AstNode *> body;

public:
    bool isEmpty() const { return body.empty(); }
    void push(AstNode *node);
    std::list<AstNode *> &getBody() { return body; }

    AstStatList() {}
    AstStatList(AstNode *node);
    void toJson(Json &root) override;

    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstTypeDecl {
    // function args declaration
public:
    std::string name;
    std::string type;
    AstTypeDecl() {}
    AstTypeDecl(AstTypeDecl &&other)
        : name(std::move(other.name)), type(std::move(other.type)) {}
    AstTypeDecl(AstToken &name, AstToken &type)
        : name(name.getText()), type(type.getText()) {}

    AstTypeDecl &operator=(AstTypeDecl &&other) {
        name = std::move(other.name);
        type = std::move(other.type);
        return *this;
    }
};

class AstFuncDecl : public AstNode {
    std::string name;
    std::list<AstTypeDecl> *argdecl = nullptr;
    AstNode *body;
    std::string retType = "void";

public:
    std::string &getName() { return name; }
    std::string &getRetType() { return retType; }
    bool hasArgs() const { return argdecl != nullptr; }
    std::list<AstTypeDecl> &getArgs() { return *argdecl; }
    void setRetType(AstToken &retType) { this->retType = retType.getText(); }

    AstFuncDecl(AstToken &name, AstNode *body,
                std::list<AstTypeDecl> *args = nullptr);
    void toJson(Json &root) override;

    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstRet : public AstNode {
    AstNode *expr;

public:
    bool hasExpr() const { return expr != nullptr; }

    AstRet(AstNode *expr);
    void toJson(Json &root) override;

    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstIf : public AstNode {
public:
    AstNode *const condition;
    AstNode *const then;
    AstNode *const els = nullptr;
    bool hasElse() const { return els != nullptr; }
    AstIf(AstNode *condition, AstNode *then, AstNode *els = nullptr);

    void toJson(Json &root) override;
    BaseVariable *accept(AstVisitor &visitor) override;
};

class AstFieldCall : public AstNode {
    std::string name;
    std::list<AstNode *> *args = nullptr;

public:
    std::string &getName() { return name; }
    bool hasArgs() const { return args != nullptr; }
    std::list<AstNode *> &getArgs() { return *args; }

    AstFieldCall(AstToken &field, std::list<AstNode *> *args = nullptr);

    void toJson(Json &root) override;
    BaseVariable *accept(AstVisitor &visitor) override;
};

}  // namespace lona