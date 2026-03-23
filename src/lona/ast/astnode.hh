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
class AstNode;
class AstIf;
class AstBreak;
class AstContinue;
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

enum class BindingKind {
    Value,
    Ref,
};

enum class AbiKind {
    Native,
    C,
};

inline const char *
abiKindKeyword(AbiKind kind) {
    return kind == AbiKind::C ? "c" : "native";
}

enum class StructDeclKind {
    Native,
    Extern,
    ReprC,
};

inline const char *
structDeclKindKeyword(StructDeclKind kind) {
    switch (kind) {
    case StructDeclKind::Extern:
        return "extern";
    case StructDeclKind::ReprC:
        return "repr_c";
    case StructDeclKind::Native:
    default:
        return "native";
    }
}

inline const char *
bindingKindKeyword(BindingKind kind) {
    return kind == BindingKind::Ref ? "ref" : "var";
}

struct TypeNode {
    location const loc;
    explicit TypeNode(const location &loc = location()) : loc(loc) {}
    virtual ~TypeNode() = default;
};

struct BaseTypeNode : public TypeNode {
    string const name;
    BaseTypeNode(string name, const location &loc = location())
        : TypeNode(loc), name(name) {}
};

struct ConstTypeNode : public TypeNode {
    TypeNode *base;

    explicit ConstTypeNode(TypeNode *base, const location &loc = location())
        : TypeNode(loc), base(base) {}
};

struct PointerTypeNode : public TypeNode {
    TypeNode *base;
    uint32_t dim;

    PointerTypeNode(TypeNode *base, uint32_t dim = 1,
                    const location &loc = location())
        : TypeNode(loc), base(base), dim(dim) {}
};

struct IndexablePointerTypeNode : public TypeNode {
    TypeNode *base;

    IndexablePointerTypeNode(TypeNode *base, const location &loc = location())
        : TypeNode(loc), base(base) {}
};

struct ArrayTypeNode : public TypeNode {
    TypeNode *base;
    std::vector<AstNode*> dim;

    ArrayTypeNode(TypeNode *base, std::vector<AstNode*> dim = {},
                  const location &loc = location())
        : TypeNode(loc), base(base), dim(std::move(dim)) {}
};

struct TupleTypeNode : public TypeNode {
    std::vector<TypeNode *> items;

    TupleTypeNode(std::vector<TypeNode *> items = {},
                  const location &loc = location())
        : TypeNode(loc), items(std::move(items)) {}
};

struct FuncPtrTypeNode : public TypeNode {
    std::vector<TypeNode*> args;
    TypeNode* ret = nullptr;

    FuncPtrTypeNode(std::vector<TypeNode*> args = {}, TypeNode* ret = nullptr,
                    const location &loc = location())
        : TypeNode(loc), args(std::move(args)), ret(ret) {}
};

struct FuncParamTypeNode : public TypeNode {
    BindingKind bindingKind = BindingKind::Value;
    TypeNode *type = nullptr;

    FuncParamTypeNode(BindingKind bindingKind, TypeNode *type,
                      const location &loc = location())
        : TypeNode(loc), bindingKind(bindingKind), type(type) {}
};

inline BindingKind
funcParamBindingKind(const TypeNode *node) {
    auto *param = dynamic_cast<const FuncParamTypeNode *>(node);
    return param ? param->bindingKind : BindingKind::Value;
}

inline TypeNode *
unwrapFuncParamType(TypeNode *node) {
    auto *param = dynamic_cast<FuncParamTypeNode *>(node);
    return param ? param->type : node;
}

inline const TypeNode *
unwrapFuncParamType(const TypeNode *node) {
    auto *param = dynamic_cast<const FuncParamTypeNode *>(node);
    return param ? param->type : node;
}

extern FuncPtrTypeNode* findFuncPtrTypeNode(TypeNode* node);
extern TypeNode* createPointerOrArrayTypeNode(TypeNode* head, std::vector<AstNode*>* suffix);

class AstNode {
public:
    location const loc;
    AstNode() {}
    AstNode(const location &loc) : loc(loc) {}

    virtual Object *accept(AstVisitor &visitor) = 0;
    virtual bool hasTerminator() { return false; }
    virtual void toJson(Json &root) {};

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
    enum class Type { INT32, FP64, STRING, BOOL };

private:
    // the type of the constant
    Type vtype;
    void *buf = nullptr;  // never delete
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

class AstFuncRef : public AstNode {
public:
    string const name;
    std::vector<TypeNode *> *const argTypes;

    AstFuncRef(AstToken &name, std::vector<TypeNode *> *argTypes)
        : AstNode(name.loc), name(name.text), argTypes(argTypes) {}

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

class AstRefExpr : public AstNode {
public:
    AstNode *const expr;

    explicit AstRefExpr(const location &loc, AstNode *expr)
        : AstNode(loc), expr(expr) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstTupleLiteral : public AstNode {
public:
    std::vector<AstNode *> *const items;

    AstTupleLiteral(const location &loc, std::vector<AstNode *> *items)
        : AstNode(loc), items(items) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstBraceInitItem : public AstNode {
public:
    AstNode *const value;

    explicit AstBraceInitItem(AstNode *value)
        : AstNode(value ? value->loc : location()), value(value) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstBraceInit : public AstNode {
public:
    std::vector<AstNode *> *const items;

    AstBraceInit(const location &loc, std::vector<AstNode *> *items)
        : AstNode(loc), items(items) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstNamedCallArg : public AstNode {
public:
    string const name;
    AstNode *const value;

    AstNamedCallArg(AstToken &nameToken, AstNode *value)
        : AstNode(nameToken.loc), name(nameToken.text), value(value) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstStructDecl : public AstNode {
public:
    string const name;
    AstNode *const body;
    StructDeclKind const declKind;

    AstStructDecl(AstToken &field, AstNode *body,
                  StructDeclKind declKind = StructDeclKind::Native)
        : AstNode(field.loc), name(field.text), body(body), declKind(declKind) {}
    bool hasBody() const { return body != nullptr; }
    bool isExternDecl() const { return declKind == StructDeclKind::Extern; }
    bool isReprC() const { return declKind == StructDeclKind::ReprC; }
    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstImport : public AstNode {
public:
    std::string const path;

    AstImport(const location &loc, AstToken &pathToken)
        : AstNode(loc),
          path(pathToken.text.tochara(), pathToken.text.size()) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstVarDecl : public AstNode {
public:
    BindingKind const bindingKind;
    string const field;
    TypeNode *const typeNode;
    AstNode *const right;

    AstVarDecl(BindingKind bindingKind, AstToken &field, TypeNode *typeNode,
               AstNode *right = nullptr);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstVarDef : public AstNode {
    BindingKind const bindingKind;
    string const field;
    TypeNode *const typeNode;
    AstNode *const initVal;
public:
    AstVarDef(AstVarDecl *vardecl, AstNode *initVal = nullptr)
        : AstNode(vardecl->loc), bindingKind(vardecl->bindingKind),
          field(vardecl->field), typeNode(vardecl->typeNode),
          initVal(initVal) {}

    AstVarDef(AstToken &field,
               AstNode *initVal = nullptr)
        : AstNode(field.loc), bindingKind(BindingKind::Value), field(field.text),
          typeNode(nullptr), initVal(initVal) {}


    auto& getName() const { return field; }
    BindingKind getBindingKind() const { return bindingKind; }
    bool isRefBinding() const { return bindingKind == BindingKind::Ref; }
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

    Object *accept(AstVisitor &visitor) override;
};

class AstFuncDecl : public AstNode {
public:
    string const name;
    std::vector<AstNode *> *const args = nullptr;
    AstNode *const body;
    TypeNode *const retType;
    AbiKind const abiKind;
    bool hasArgs() const { return args != nullptr; }
    bool hasBody() const { return body != nullptr; }
    bool isExternC() const { return abiKind == AbiKind::C; }

    AstFuncDecl(AstToken &name, AstNode *body,
                std::vector<AstNode *> *args = nullptr,
                TypeNode *retType = nullptr,
                AbiKind abiKind = AbiKind::Native);
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstRet : public AstNode {
public:
    AstNode *const expr = nullptr;

    AstRet(const location &loc, AstNode *expr);
    void toJson(Json &root) override;

    bool hasTerminator() override {
        return true;
    }

    Object *accept(AstVisitor &visitor) override;
};

class AstBreak : public AstNode {
public:
    explicit AstBreak(const location &loc) : AstNode(loc) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstContinue : public AstNode {
public:
    explicit AstContinue(const location &loc) : AstNode(loc) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstIf : public AstNode {
public:
    AstNode *const condition;
    AstNode *const then;
    AstNode *const els = nullptr;
    bool hasElse() const { return els != nullptr; }

    AstIf(AstNode *condition, AstNode *then, AstNode *els = nullptr);

    bool hasTerminator() override {
        if (els == nullptr) {
            return false;
        }
        return then->hasTerminator() && els->hasTerminator();
    }

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstFor : public AstNode {
public:
    AstNode *const expr;
    AstNode *const body;
    AstNode *const els = nullptr;
    bool hasElse() const { return els != nullptr; }

    AstFor(AstNode *expr, AstNode *body, AstNode *els = nullptr);

    bool hasTerminator() override {
        if (els == nullptr) {
            return false;
        }
        return body->hasTerminator() && els->hasTerminator();
    }

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstFieldCall : public AstNode {
public:
    // Generic parenthesis application node. The concrete meaning of `xxx(...)`
    // is decided later during semantic analysis.
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
        : AstNode(field->loc), parent(parent), field(field) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

}  // namespace lona
