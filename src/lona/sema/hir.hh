#pragma once

#include "lona/ast/astnode.hh"
#include "lona/sema/injected_member.hh"
#include "lona/sema/operator_resolver.hh"
#include "lona/support/arena.hh"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lona {

inline constexpr char kMethodSelectorDirectCallError[] =
    "method selector can only be used as a direct call callee";

class GlobalScope;
class ResolvedModule;
class CompilationUnit;

struct HIRBinding {
    std::string name;
    BindingKind bindingKind = BindingKind::Value;
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

class HIRTupleLiteral : public HIRExpr {
    std::vector<HIRExpr *> items;

public:
    HIRTupleLiteral(std::vector<HIRExpr *> items, TypeClass *type = nullptr,
                    const location &loc = location())
        : HIRExpr(type, loc), items(std::move(items)) {}

    const std::vector<HIRExpr *> &getItems() const { return items; }
};

class HIRStructLiteral : public HIRExpr {
    std::vector<HIRExpr *> fields;

public:
    HIRStructLiteral(std::vector<HIRExpr *> fields, TypeClass *type = nullptr,
                     const location &loc = location())
        : HIRExpr(type, loc), fields(std::move(fields)) {}

    const std::vector<HIRExpr *> &getFields() const { return fields; }
};

class HIRArrayInit : public HIRExpr {
    std::vector<HIRExpr *> items;

public:
    HIRArrayInit(std::vector<HIRExpr *> items = {}, TypeClass *type = nullptr,
                 const location &loc = location())
        : HIRExpr(type, loc), items(std::move(items)) {}

    const std::vector<HIRExpr *> &getItems() const { return items; }
};

class HIRNumericCast : public HIRExpr {
    HIRExpr *expr;
    bool explicitRequest_ = false;

public:
    HIRNumericCast(HIRExpr *expr, TypeClass *targetType, bool explicitRequest,
                   const location &loc = location())
        : HIRExpr(targetType, loc), expr(expr), explicitRequest_(explicitRequest) {}

    HIRExpr *getExpr() const { return expr; }
    bool explicitRequest() const { return explicitRequest_; }
};

class HIRBitCast : public HIRExpr {
    HIRExpr *expr;

public:
    HIRBitCast(HIRExpr *expr, TypeClass *targetType, const location &loc = location())
        : HIRExpr(targetType, loc), expr(expr) {}

    HIRExpr *getExpr() const { return expr; }
};

class HIRUnaryOper : public HIRExpr {
    UnaryOperatorBinding binding_;
    HIRExpr *expr;

public:
    HIRUnaryOper(UnaryOperatorBinding binding, HIRExpr *expr,
                 TypeClass *type = nullptr,
                 const location &loc = location())
        : HIRExpr(type ? type : binding.resultType, loc),
          binding_(std::move(binding)),
          expr(expr) {}

    token_type getOp() const { return binding_.token; }
    const UnaryOperatorBinding &getBinding() const { return binding_; }
    HIRExpr *getExpr() const { return expr; }
};

class HIRBinOper : public HIRExpr {
    BinaryOperatorBinding binding_;
    HIRExpr *left;
    HIRExpr *right;

public:
    HIRBinOper(BinaryOperatorBinding binding, HIRExpr *left, HIRExpr *right,
               TypeClass *type = nullptr, const location &loc = location())
        : HIRExpr(type ? type : binding.resultType, loc),
          binding_(std::move(binding)),
          left(left),
          right(right) {}

    token_type getOp() const { return binding_.token; }
    const BinaryOperatorBinding &getBinding() const { return binding_; }
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

enum class HIRSelectorKind {
    ValueField,
    Method,
};

class HIRSelector : public HIRExpr {
    HIRExpr *parent;
    std::string fieldName;
    HIRSelectorKind kind_;

public:
    HIRSelector(HIRExpr *parent, std::string fieldName, TypeClass *type = nullptr,
                const location &loc = location(),
                HIRSelectorKind kind = HIRSelectorKind::ValueField)
        : HIRExpr(type, loc),
          parent(parent),
          fieldName(std::move(fieldName)),
          kind_(kind) {}

    HIRExpr *getParent() const { return parent; }
    const std::string &getFieldName() const { return fieldName; }
    HIRSelectorKind getKind() const { return kind_; }
    bool isValueFieldSelector() const {
        return kind_ == HIRSelectorKind::ValueField;
    }
    bool isMethodSelector() const { return kind_ == HIRSelectorKind::Method; }
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

class HIRIndex : public HIRExpr {
    HIRExpr *target;
    std::vector<HIRExpr *> indices;

public:
    HIRIndex(HIRExpr *target, std::vector<HIRExpr *> indices,
             TypeClass *type = nullptr, const location &loc = location())
        : HIRExpr(type, loc), target(target), indices(std::move(indices)) {}

    HIRExpr *getTarget() const { return target; }
    const std::vector<HIRExpr *> &getIndices() const { return indices; }
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
    Arena arena_;
    std::vector<HIRFunc *> funcs;

public:
    template<typename T, typename... Args>
    T *create(Args &&...args) {
        return arena_.emplace<T>(std::forward<Args>(args)...);
    }

    const std::vector<HIRFunc *> &getFunctions() const { return funcs; }
    void addFunction(HIRFunc *func) {
        if (func) {
            funcs.push_back(func);
        }
    }
};

std::unique_ptr<HIRModule>
analyzeModule(GlobalScope *global, const ResolvedModule &resolved,
              const CompilationUnit *unit = nullptr);

}  // namespace lona
