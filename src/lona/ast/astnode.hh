#pragma once

#include <cassert>
#include <cstdint>
#include <list>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../ast/token.hh"
#include "../err/err.hh"
#include "../sym/object.hh"
#include "../util/string.hh"
#include "location.hh"

using Json = nlohmann::ordered_json;

namespace lona {
class AstNode;
class AstIf;
class AstBreak;
class AstContinue;
class AstRet;
class AstGenericParam;
class AstTraitDecl;
class AstTraitImplDecl;
class AstVisitor;
class Object;
class Scope;
class Function;

class TypeClass;
class TypeTable;

using token_type = int;

template<typename T>
inline void
deletePointerVector(std::vector<T *> *items) {
    if (!items) {
        return;
    }
    for (auto *item : *items) {
        delete item;
    }
    delete items;
}

const int pointerType_pointer = 1;
const int pointerType_autoArray = 2;
const int pointerType_fixedArray = 3;

enum class BindingKind {
    Value,
    Ref,
};

enum class VarStorageKind {
    Var,
    Const,
    Inline,
};

enum class AbiKind {
    Native,
    C,
};

inline const char *
abiKindKeyword(AbiKind kind) {
    return kind == AbiKind::C ? "c" : "native";
}

enum class AccessKind {
    GetOnly,
    GetSet,
};

inline const char *
accessKindKeyword(AccessKind kind) {
    return kind == AccessKind::GetSet ? "set" : "get";
}

enum class StructDeclKind {
    Native,
    Opaque,
    ReprC,
};

enum class AstKind {
    Program,
    TagNode,
    StatList,
    Const,
    Field,
    FuncRef,
    Assign,
    BinOper,
    UnaryOper,
    RefExpr,
    TupleLiteral,
    BraceInitItem,
    BraceInit,
    NamedCallArg,
    TypeApply,
    StructDecl,
    TraitDecl,
    TraitImplDecl,
    GlobalDecl,
    Import,
    VarDecl,
    VarDef,
    FuncDecl,
    Ret,
    Break,
    Continue,
    If,
    For,
    CastExpr,
    SizeofExpr,
    FieldCall,
    DotLike,
};

inline const char *
structDeclKindKeyword(StructDeclKind kind) {
    switch (kind) {
        case StructDeclKind::Opaque:
            return "opaque";
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

inline const char *
varStorageKindKeyword(VarStorageKind kind) {
    switch (kind) {
        case VarStorageKind::Var:
            return "var";
        case VarStorageKind::Const:
            return "const";
        case VarStorageKind::Inline:
            return "inline";
    }
    return "var";
}

class AstTag {
public:
    AstToken const name;
    std::vector<AstToken> *const args = nullptr;

    explicit AstTag(AstToken &name, std::vector<AstToken *> *args = nullptr)
        : name(name), args(new std::vector<AstToken>) {
        if (!args) {
            return;
        }
        this->args->reserve(args->size());
        for (auto *arg : *args) {
            if (arg) {
                this->args->push_back(*arg);
            }
        }
        delete args;
    }
    ~AstTag();

    void toJson(Json &root) const;
};

class AstGenericParam {
public:
    AstToken const name;
    AstNode *const boundTrait = nullptr;

    explicit AstGenericParam(AstToken &name, AstNode *boundTrait = nullptr)
        : name(name), boundTrait(boundTrait) {}
    ~AstGenericParam();

    bool hasBoundTrait() const { return boundTrait != nullptr; }
    void toJson(Json &root) const;
};

struct TypeNode {
    location const loc;
    explicit TypeNode(const location &loc = location()) : loc(loc) {}
    virtual ~TypeNode() = default;
};

struct AnyTypeNode : public TypeNode {
    explicit AnyTypeNode(const location &loc = location()) : TypeNode(loc) {}
};

struct BaseTypeNode : public TypeNode {
    string name;
    AstNode *syntax = nullptr;

    BaseTypeNode(string name, const location &loc = location())
        : TypeNode(loc), name(name) {}
    BaseTypeNode(AstNode *syntax, const location &loc = location())
        : TypeNode(loc), syntax(syntax) {}
    ~BaseTypeNode() override;

    bool hasSyntax() const { return syntax != nullptr; }
};

struct AppliedTypeNode : public TypeNode {
    TypeNode *base;
    std::vector<TypeNode *> args;

    AppliedTypeNode(TypeNode *base, std::vector<TypeNode *> args = {},
                    const location &loc = location())
        : TypeNode(loc), base(base), args(std::move(args)) {}
    ~AppliedTypeNode() override;
};

struct DynTypeNode : public TypeNode {
    TypeNode *base;

    explicit DynTypeNode(TypeNode *base, const location &loc = location())
        : TypeNode(loc), base(base) {}
    ~DynTypeNode() override;
};

struct ConstTypeNode : public TypeNode {
    TypeNode *base;

    explicit ConstTypeNode(TypeNode *base, const location &loc = location())
        : TypeNode(loc), base(base) {}
    ~ConstTypeNode() override;
};

struct PointerTypeNode : public TypeNode {
    TypeNode *base;
    uint32_t dim;

    PointerTypeNode(TypeNode *base, uint32_t dim = 1,
                    const location &loc = location())
        : TypeNode(loc), base(base), dim(dim) {}
    ~PointerTypeNode() override;
};

struct IndexablePointerTypeNode : public TypeNode {
    TypeNode *base;

    IndexablePointerTypeNode(TypeNode *base, const location &loc = location())
        : TypeNode(loc), base(base) {}
    ~IndexablePointerTypeNode() override;
};

struct ArrayTypeNode : public TypeNode {
    TypeNode *base;
    std::vector<AstNode *> dim;

    ArrayTypeNode(TypeNode *base, std::vector<AstNode *> dim = {},
                  const location &loc = location())
        : TypeNode(loc), base(base), dim(std::move(dim)) {}
    ~ArrayTypeNode() override;
};

struct TupleTypeNode : public TypeNode {
    std::vector<TypeNode *> items;

    TupleTypeNode(std::vector<TypeNode *> items = {},
                  const location &loc = location())
        : TypeNode(loc), items(std::move(items)) {}
    ~TupleTypeNode() override;
};

struct FuncPtrTypeNode : public TypeNode {
    std::vector<TypeNode *> args;
    TypeNode *ret = nullptr;

    FuncPtrTypeNode(std::vector<TypeNode *> args = {}, TypeNode *ret = nullptr,
                    const location &loc = location())
        : TypeNode(loc), args(std::move(args)), ret(ret) {}
    ~FuncPtrTypeNode() override;
};

struct FuncParamTypeNode : public TypeNode {
    BindingKind bindingKind = BindingKind::Value;
    TypeNode *type = nullptr;

    FuncParamTypeNode(BindingKind bindingKind, TypeNode *type,
                      const location &loc = location())
        : TypeNode(loc), bindingKind(bindingKind), type(type) {}
    ~FuncParamTypeNode() override;
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

extern FuncPtrTypeNode *
findFuncPtrTypeNode(TypeNode *node);
extern TypeNode *
createPointerOrArrayTypeNode(TypeNode *head, std::vector<AstNode *> *suffix);

class AstNode {
public:
    location const loc;
    explicit AstNode(AstKind kind, const location &loc = location())
        : loc(loc), kind_(kind) {}
    virtual ~AstNode() = default;

    AstKind kind() const { return kind_; }

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

private:
    AstKind kind_;
};

class AstTagNode : public AstNode {
public:
    std::vector<AstTag *> *tags;

    explicit AstTagNode(std::vector<AstTag *> *tags)
        : AstNode(AstKind::TagNode,
                  tags && !tags->empty() && (*tags)[0]
                      ? (*tags)[0]->name.loc
                      : location()),
          tags(tags ? tags : new std::vector<AstTag *>) {}
    ~AstTagNode() override;
    std::vector<AstTag *> *releaseTags() {
        auto *released = tags;
        tags = new std::vector<AstTag *>;
        return released;
    }

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstStatList;

class AstProgram : public AstNode {
public:
    AstStatList *const body;
    AstProgram(AstNode *body);
    ~AstProgram() override;
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstConst : public AstNode {
public:
    enum class Type {
        I8,
        U8,
        I16,
        U16,
        I32,
        U32,
        I64,
        U64,
        USIZE,
        F32,
        F64,
        STRING,
        CHAR,
        BOOL,
        NULLPTR,
    };

private:
    // the type of the constant
    Type vtype;
    bool explicitNumericType = false;
    bool requiresUnaryMinusForSignedMin = false;
    void *buf = nullptr;  // never delete

    void setNumericLiteral(Type type, bool explicitType, void *value,
                           bool unaryMinusOnly = false);

public:
    Type getType() const { return vtype; }
    bool hasExplicitNumericType() const { return explicitNumericType; }
    bool isUnaryMinusOnlySignedMinLiteral() const {
        return requiresUnaryMinusForSignedMin;
    }
    bool isDefaultFloatLiteral() const {
        return vtype == Type::F64 && !explicitNumericType;
    }
    bool isIntegerLiteral() const {
        switch (vtype) {
            case Type::I8:
            case Type::U8:
            case Type::I16:
            case Type::U16:
            case Type::I32:
            case Type::U32:
            case Type::I64:
            case Type::U64:
            case Type::USIZE:
                return true;
            default:
                return false;
        }
    }
    bool isFloatLiteral() const {
        return vtype == Type::F32 || vtype == Type::F64;
    }
    template<typename T = char>
    T *getBuf() const {
        return (T *)buf;
    }
    std::uint64_t getDeferredSignedMinMagnitude() const {
        return isUnaryMinusOnlySignedMinLiteral() ? *getBuf<std::uint64_t>()
                                                  : 0;
    }

    AstConst(AstToken &token);
    ~AstConst() override;
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstField : public AstNode {
public:
    string const name;
    AstField(AstToken &token);
    AstField(string name, const location &loc = location())
        : AstNode(AstKind::Field, loc), name(name) {}
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstFuncRef : public AstNode {
public:
    AstNode *const value;

    AstFuncRef(AstNode *value, const location &loc = location())
        : AstNode(AstKind::FuncRef, loc), value(value) {}
    ~AstFuncRef() override;

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstAssign : public AstNode {
public:
    AstNode *const left;
    AstNode *const right;
    AstAssign(AstNode *left, AstNode *right);
    ~AstAssign() override;
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstBinOper : public AstNode {
public:
    AstNode *const left;
    token_type const op;
    AstNode *const right;
    bool const ownsLeft;
    bool const ownsRight;
    AstBinOper(AstNode *left, token_type op, AstNode *right,
               bool ownsLeft = true, bool ownsRight = true);
    ~AstBinOper() override;
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstUnaryOper : public AstNode {
public:
    token_type const op;
    AstNode *const expr;

    AstUnaryOper(token_type op, AstNode *expr);
    ~AstUnaryOper() override;
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstRefExpr : public AstNode {
public:
    AstNode *const expr;

    explicit AstRefExpr(const location &loc, AstNode *expr)
        : AstNode(AstKind::RefExpr, loc), expr(expr) {}
    ~AstRefExpr() override;

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstTupleLiteral : public AstNode {
public:
    std::vector<AstNode *> *const items;

    AstTupleLiteral(const location &loc, std::vector<AstNode *> *items)
        : AstNode(AstKind::TupleLiteral, loc), items(items) {}
    ~AstTupleLiteral() override;

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstBraceInitItem : public AstNode {
public:
    AstNode *const value;

    explicit AstBraceInitItem(AstNode *value)
        : AstNode(AstKind::BraceInitItem, value ? value->loc : location()),
          value(value) {}
    ~AstBraceInitItem() override;

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstBraceInit : public AstNode {
public:
    std::vector<AstNode *> *const items;

    AstBraceInit(const location &loc, std::vector<AstNode *> *items)
        : AstNode(AstKind::BraceInit, loc), items(items) {}
    ~AstBraceInit() override;

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstNamedCallArg : public AstNode {
public:
    string const name;
    AstNode *const value;

    AstNamedCallArg(AstToken &nameToken, AstNode *value)
        : AstNode(AstKind::NamedCallArg, nameToken.loc),
          name(nameToken.text),
          value(value) {}
    ~AstNamedCallArg() override;

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstTypeApply : public AstNode {
public:
    AstNode *const value;
    std::vector<TypeNode *> *const typeArgs;

    AstTypeApply(AstNode *value, std::vector<TypeNode *> *typeArgs,
                 const location &loc = location())
        : AstNode(AstKind::TypeApply, loc), value(value), typeArgs(typeArgs) {}
    ~AstTypeApply() override;

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstStructDecl : public AstNode {
public:
    string const name;
    std::vector<AstGenericParam *> *const typeParams = nullptr;
    AstNode *const body;
    StructDeclKind declKind;

    AstStructDecl(AstToken &field, AstNode *body,
                  std::vector<AstGenericParam *> *typeParams = nullptr,
                  StructDeclKind declKind = StructDeclKind::Native)
        : AstNode(AstKind::StructDecl, field.loc),
          name(field.text),
          typeParams(typeParams),
          body(body),
          declKind(declKind) {}
    ~AstStructDecl() override;
    bool hasTypeParams() const {
        return typeParams != nullptr && !typeParams->empty();
    }
    bool hasBody() const { return body != nullptr; }
    bool isOpaqueDecl() const { return declKind == StructDeclKind::Opaque; }
    bool isReprC() const { return declKind == StructDeclKind::ReprC; }
    void setDeclKind(StructDeclKind kind) { declKind = kind; }
    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstTraitDecl : public AstNode {
public:
    string const name;
    AstNode *const body;

    AstTraitDecl(AstToken &field, AstNode *body)
        : AstNode(AstKind::TraitDecl, field.loc), name(field.text), body(body) {}
    ~AstTraitDecl() override;

    bool hasBody() const { return body != nullptr; }
    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstTraitImplDecl : public AstNode {
public:
    std::vector<AstGenericParam *> *typeParams = nullptr;
    TypeNode *selfType;
    AstNode *const trait;
    AstNode *const body;

    AstTraitImplDecl(TypeNode *selfType, AstNode *trait, AstNode *body,
                     std::vector<AstGenericParam *> *typeParams = nullptr,
                     const location &loc = location())
        : AstNode(AstKind::TraitImplDecl, loc),
          typeParams(typeParams),
          selfType(selfType),
          trait(trait),
          body(body) {}
    ~AstTraitImplDecl() override;

    bool hasTypeParams() const {
        return typeParams != nullptr && !typeParams->empty();
    }
    bool hasBody() const { return body != nullptr; }
    bool hasSelfType() const { return selfType != nullptr; }
    void setSelfType(TypeNode *value) { selfType = value; }
    void setTypeParams(std::vector<AstGenericParam *> *value) {
        typeParams = value;
    }
    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstGlobalDecl : public AstNode {
    string name_;
    TypeNode *const typeNode_;
    AstNode *const initVal_;
    bool externLinkage_ = false;

public:
    AstGlobalDecl(AstToken &name, TypeNode *typeNode = nullptr,
                  AstNode *initVal = nullptr, bool isExtern = false)
        : AstNode(AstKind::GlobalDecl, name.loc),
          name_(name.text),
          typeNode_(typeNode),
          initVal_(initVal),
          externLinkage_(isExtern) {}
    ~AstGlobalDecl() override;

    const string &getName() const { return name_; }
    TypeNode *getTypeNode() const { return typeNode_; }
    AstNode *getInitVal() const { return initVal_; }
    bool hasTypeNode() const { return typeNode_ != nullptr; }
    bool hasInitVal() const { return initVal_ != nullptr; }
    bool isExtern() const { return externLinkage_; }
    void setExtern(bool value = true) { externLinkage_ = value; }

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstImport : public AstNode {
public:
    std::string const path;

    AstImport(const location &loc, AstToken &pathToken)
        : AstNode(AstKind::Import, loc),
          path(pathToken.text.tochara(), pathToken.text.size()) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstVarDecl : public AstNode {
public:
    BindingKind const bindingKind;
    AccessKind const accessKind;
    string const field;
    bool const embeddedField;
    TypeNode *typeNode;
    AstNode *right;

    AstVarDecl(BindingKind bindingKind, AstToken &field, TypeNode *typeNode,
               AstNode *right = nullptr,
               AccessKind accessKind = AccessKind::GetOnly,
               bool embeddedField = false);
    ~AstVarDecl() override;
    TypeNode *takeTypeNode() {
        auto *released = typeNode;
        typeNode = nullptr;
        return released;
    }
    AstNode *takeRight() {
        auto *released = right;
        right = nullptr;
        return released;
    }
    bool isEmbeddedField() const { return embeddedField; }
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstVarDef : public AstNode {
    BindingKind const bindingKind;
    VarStorageKind const storageKind;
    string const field;
    TypeNode *const typeNode;
    AstNode *const initVal;

public:
    AstVarDef(AstVarDecl *vardecl, AstNode *initVal = nullptr,
              VarStorageKind storageKind = VarStorageKind::Var);

    AstVarDef(AstToken &field, AstNode *initVal = nullptr,
              VarStorageKind storageKind = VarStorageKind::Var)
        : AstNode(AstKind::VarDef, field.loc),
          bindingKind(BindingKind::Value),
          storageKind(storageKind),
          field(field.text),
          typeNode(nullptr),
          initVal(initVal) {}
    ~AstVarDef() override;

    auto &getName() const { return field; }
    BindingKind getBindingKind() const { return bindingKind; }
    bool isRefBinding() const { return bindingKind == BindingKind::Ref; }
    VarStorageKind getStorageKind() const { return storageKind; }
    bool isReadOnlyBinding() const {
        return storageKind == VarStorageKind::Const ||
               storageKind == VarStorageKind::Inline;
    }
    bool isConstBinding() const { return storageKind == VarStorageKind::Const; }
    bool isInlineBinding() const {
        return storageKind == VarStorageKind::Inline;
    }
    TypeNode *getTypeNode() const { return typeNode; }
    AstNode *getInitVal() const { return initVal; }

    bool withInitVal() const { return initVal != nullptr; }

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstStatList : public AstNode {
public:
    std::list<AstNode *> body;
    bool ownsElements = true;
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

    explicit AstStatList(bool ownsElements = true)
        : AstNode(AstKind::StatList), ownsElements(ownsElements) {}
    AstStatList(AstNode *node, bool ownsElements = true);
    ~AstStatList() override;
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstFuncDecl : public AstNode {
public:
    string const name;
    std::vector<AstGenericParam *> *const typeParams = nullptr;
    std::vector<AstNode *> *const args = nullptr;
    AstNode *const body;
    TypeNode *const retType;
    AbiKind abiKind;
    AccessKind receiverAccess = AccessKind::GetOnly;
    bool extensionMethod = false;
    bool hasTypeParams() const {
        return typeParams != nullptr && !typeParams->empty();
    }
    bool hasArgs() const { return args != nullptr; }
    bool hasBody() const { return body != nullptr; }
    bool isExternC() const { return abiKind == AbiKind::C; }
    bool hasExtensionReceiver() const { return extensionMethod; }
    AstVarDecl *extensionReceiverParam() const {
        if (!extensionMethod || !args || args->empty()) {
            return nullptr;
        }
        return dynamic_cast<AstVarDecl *>(args->front());
    }
    TypeNode *extensionReceiverType() const {
        auto *param = extensionReceiverParam();
        return param ? param->typeNode : nullptr;
    }
    void setAbiKind(AbiKind kind) { abiKind = kind; }

    AstFuncDecl(AstToken &name, AstNode *body,
                std::vector<AstGenericParam *> *typeParams = nullptr,
                std::vector<AstNode *> *args = nullptr,
                TypeNode *retType = nullptr, AbiKind abiKind = AbiKind::Native,
                AccessKind receiverAccess = AccessKind::GetOnly,
                bool extensionMethod = false);
    ~AstFuncDecl() override;
    void toJson(Json &root) override;

    Object *accept(AstVisitor &visitor) override;
};

class AstRet : public AstNode {
public:
    AstNode *const expr = nullptr;

    AstRet(const location &loc, AstNode *expr);
    ~AstRet() override;
    void toJson(Json &root) override;

    bool hasTerminator() override { return true; }

    Object *accept(AstVisitor &visitor) override;
};

class AstBreak : public AstNode {
public:
    explicit AstBreak(const location &loc) : AstNode(AstKind::Break, loc) {}

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstContinue : public AstNode {
public:
    explicit AstContinue(const location &loc)
        : AstNode(AstKind::Continue, loc) {}

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
    ~AstIf() override;

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
    ~AstFor() override;

    bool hasTerminator() override {
        if (els == nullptr) {
            return false;
        }
        return body->hasTerminator() && els->hasTerminator();
    }

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstCastExpr : public AstNode {
public:
    TypeNode *const targetType;
    AstNode *const value;

    AstCastExpr(TypeNode *targetType, AstNode *value,
                const location &loc = location())
        : AstNode(AstKind::CastExpr, loc), targetType(targetType), value(value) {}
    ~AstCastExpr() override;

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstSizeofExpr : public AstNode {
public:
    TypeNode *const targetType = nullptr;
    AstNode *const value = nullptr;

    AstSizeofExpr(TypeNode *targetType, AstNode *value,
                  const location &loc = location())
        : AstNode(AstKind::SizeofExpr, loc),
          targetType(targetType),
          value(value) {}
    ~AstSizeofExpr() override;

    bool hasTypeOperand() const { return targetType != nullptr; }
    bool hasValueOperand() const { return value != nullptr; }

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
    ~AstFieldCall() override;

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

class AstDotLike : public AstNode {
public:
    AstNode *const parent;
    AstToken const field;
    AstDotLike(AstNode *parent, AstToken *field)
        : AstNode(AstKind::DotLike, field ? field->loc : location()),
          parent(parent),
          field(field ? *field : AstToken()) {}
    ~AstDotLike() override;

    void toJson(Json &root) override;
    Object *accept(AstVisitor &visitor) override;
};

std::string
describeDotLikeSyntax(const AstNode *node,
                      std::string_view nullDescription = "");
bool
collectDotLikeSegments(const AstNode *node, std::vector<std::string> &segments);

}  // namespace lona
