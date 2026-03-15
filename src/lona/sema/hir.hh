#pragma once

#include "lona/ast/astnode.hh"
#include <string>
#include <utility>
#include <vector>

namespace lona {

inline constexpr char kMethodSelectorDirectCallError[] =
    "method selector can only be used as a direct call callee";

class GlobalScope;
class ResolvedModule;

struct HIRBinding {
    std::string name;
    ObjectPtr object = nullptr;
    location loc;
};

class HIRNode {
    location loc;

public:
    explicit HIRNode(const location &loc = location()) : loc(loc) {}
    virtual ~HIRNode() = default;

    const location &getLocation() const { return loc; }
};

class HIRExpr : public HIRNode {
    TypeClass *type = nullptr;

public:
    explicit HIRExpr(TypeClass *type = nullptr, const location &loc = location())
        : HIRNode(loc), type(type) {}

    TypeClass *getType() const { return type; }
    void setType(TypeClass *value) { type = value; }
};

class HIRValue : public HIRExpr {
    ObjectPtr value;

public:
    explicit HIRValue(ObjectPtr value, const location &loc = location())
        : HIRExpr(value ? value->getType() : nullptr, loc), value(value) {}

    ObjectPtr getValue() const { return value; }
};

class HIRUnaryOper : public HIRExpr {
    token_type op;
    HIRExpr *expr;

public:
    HIRUnaryOper(token_type op, HIRExpr *expr, TypeClass *type = nullptr,
                 const location &loc = location())
        : HIRExpr(type, loc), op(op), expr(expr) {}

    token_type getOp() const { return op; }
    HIRExpr *getExpr() const { return expr; }
};

class HIRBinOper : public HIRExpr {
    token_type op;
    HIRExpr *left;
    HIRExpr *right;

public:
    HIRBinOper(token_type op, HIRExpr *left, HIRExpr *right,
               TypeClass *type = nullptr, const location &loc = location())
        : HIRExpr(type, loc), op(op), left(left), right(right) {}

    token_type getOp() const { return op; }
    HIRExpr *getLeft() const { return left; }
    HIRExpr *getRight() const { return right; }
};

class HIRAssign : public HIRExpr {
    HIRExpr *left;
    HIRExpr *right;

public:
    HIRAssign(HIRExpr *left, HIRExpr *right, const location &loc = location())
        : HIRExpr(left ? left->getType() : nullptr, loc), left(left), right(right) {}

    HIRAssign(ObjectPtr left, ObjectPtr right, const location &loc = location())
        : HIRAssign(new HIRValue(left, loc), new HIRValue(right, loc), loc) {}

    HIRExpr *getLeft() const { return left; }
    HIRExpr *getRight() const { return right; }
};

class HIRSelector : public HIRExpr {
    HIRExpr *parent;
    std::string fieldName;

public:
    HIRSelector(HIRExpr *parent, std::string fieldName, TypeClass *type = nullptr,
                const location &loc = location())
        : HIRExpr(type, loc), parent(parent), fieldName(std::move(fieldName)) {}

    HIRExpr *getParent() const { return parent; }
    const std::string &getFieldName() const { return fieldName; }
};

class HIRCall : public HIRExpr {
    HIRExpr *callee;
    std::vector<HIRExpr *> args;

public:
    HIRCall(HIRExpr *callee, std::vector<HIRExpr *> args,
            TypeClass *type = nullptr, const location &loc = location())
        : HIRExpr(type, loc), callee(callee), args(std::move(args)) {}

    HIRExpr *getCallee() const { return callee; }
    const std::vector<HIRExpr *> &getArgs() const { return args; }
};

class HIRVarDef : public HIRNode {
    std::string name;
    ObjectPtr object;
    HIRExpr *init = nullptr;

public:
    HIRVarDef(std::string name, ObjectPtr object, HIRExpr *init = nullptr,
              const location &loc = location())
        : HIRNode(loc), name(std::move(name)), object(object), init(init) {}

    const std::string &getName() const { return name; }
    ObjectPtr getObject() const { return object; }
    HIRExpr *getInit() const { return init; }
};

class HIRRet : public HIRNode {
    HIRExpr *expr = nullptr;

public:
    explicit HIRRet(HIRExpr *expr = nullptr, const location &loc = location())
        : HIRNode(loc), expr(expr) {}

    HIRExpr *getExpr() const { return expr; }
};

class HIRBlock : public HIRNode {
    std::vector<HIRNode *> body;

public:
    explicit HIRBlock(const location &loc = location()) : HIRNode(loc) {}

    const std::vector<HIRNode *> &getBody() const { return body; }
    void push(HIRNode *node) {
        if (node) {
            body.push_back(node);
        }
    }
};

class HIRIf : public HIRNode {
    HIRExpr *cond;
    HIRBlock *thenBlock;
    HIRBlock *elseBlock = nullptr;

public:
    explicit HIRIf(HIRExpr *cond, const location &loc = location())
        : HIRNode(loc), cond(cond), thenBlock(new HIRBlock(loc)) {}

    explicit HIRIf(ObjectPtr cond, const location &loc = location())
        : HIRIf(new HIRValue(cond, loc), loc) {}

    HIRIf(HIRExpr *cond, HIRBlock *thenBlock, HIRBlock *elseBlock = nullptr,
          const location &loc = location())
        : HIRNode(loc), cond(cond), thenBlock(thenBlock), elseBlock(elseBlock) {}

    HIRExpr *getCondition() const { return cond; }
    HIRBlock *getThenBlock() const { return thenBlock; }
    HIRBlock *getElseBlock() const { return elseBlock; }
    bool hasElseBlock() const { return elseBlock != nullptr; }

    void pushThenNode(HIRNode *node) { thenBlock->push(node); }

    void pushElseNode(HIRNode *node) {
        if (!elseBlock) {
            elseBlock = new HIRBlock(getLocation());
        }
        elseBlock->push(node);
    }
};

class HIRFor : public HIRNode {
    HIRExpr *cond;
    HIRBlock *body;

public:
    HIRFor(HIRExpr *cond, HIRBlock *body, const location &loc = location())
        : HIRNode(loc), cond(cond), body(body) {}

    HIRExpr *getCondition() const { return cond; }
    HIRBlock *getBody() const { return body; }
};

class HIRFunc : public HIRNode {
    llvm::Function *llvmFunction;
    FuncType *funcType;
    std::vector<HIRBinding> params;
    HIRBinding self;
    bool hasSelf = false;
    HIRBlock *body = nullptr;
    bool topLevelEntry = false;
    bool guaranteedReturn = false;

public:
    HIRFunc(llvm::Function *llvmFunction, FuncType *funcType,
            const location &loc = location(), bool topLevelEntry = false,
            bool guaranteedReturn = false)
        : HIRNode(loc), llvmFunction(llvmFunction), funcType(funcType),
          topLevelEntry(topLevelEntry), guaranteedReturn(guaranteedReturn) {}

    llvm::Function *getLLVMFunction() const { return llvmFunction; }
    FuncType *getFuncType() const { return funcType; }
    const std::vector<HIRBinding> &getParams() const { return params; }
    void addParam(HIRBinding binding) { params.push_back(std::move(binding)); }

    bool hasSelfBinding() const { return hasSelf; }
    const HIRBinding &getSelfBinding() const { return self; }
    void setSelfBinding(HIRBinding binding) {
        self = std::move(binding);
        hasSelf = true;
    }

    HIRBlock *getBody() const { return body; }
    void setBody(HIRBlock *value) { body = value; }

    bool isTopLevelEntry() const { return topLevelEntry; }
    bool hasGuaranteedReturn() const { return guaranteedReturn; }
};

class HIRModule {
    std::vector<HIRFunc *> funcs;

public:
    const std::vector<HIRFunc *> &getFunctions() const { return funcs; }
    void addFunction(HIRFunc *func) {
        if (func) {
            funcs.push_back(func);
        }
    }
};

HIRModule *analyzeModule(GlobalScope *global, const ResolvedModule &resolved);

}  // namespace lona
