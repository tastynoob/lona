#pragma once

#include "lona/ast/astnode.hh"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lona {

class GlobalScope;
class CompilationUnit;

class ResolvedLocalBinding {
public:
    enum class Kind {
        Parameter,
        Variable,
        Self,
    };

private:
    Kind kind_;
    std::string name_;
    const AstNode *node_ = nullptr;
    location loc_;

public:
    ResolvedLocalBinding(Kind kind, std::string name, const AstNode *node,
                         const location &loc)
        : kind_(kind), name_(std::move(name)), node_(node), loc_(loc) {}

    Kind kind() const { return kind_; }
    const std::string &name() const { return name_; }
    const AstNode *node() const { return node_; }
    const location &loc() const { return loc_; }

    const AstVarDecl *parameterDecl() const {
        return dynamic_cast<const AstVarDecl *>(node_);
    }

    const AstVarDef *variableDecl() const {
        return dynamic_cast<const AstVarDef *>(node_);
    }
};

class ResolvedValueRef {
public:
    enum class Kind {
        LocalBinding,
        GlobalObject,
    };

private:
    Kind kind_ = Kind::GlobalObject;
    const ResolvedLocalBinding *localBinding_ = nullptr;
    std::string globalName_;

public:
    static ResolvedValueRef local(const ResolvedLocalBinding *binding) {
        ResolvedValueRef ref;
        ref.kind_ = Kind::LocalBinding;
        ref.localBinding_ = binding;
        return ref;
    }

    static ResolvedValueRef global(std::string name) {
        ResolvedValueRef ref;
        ref.kind_ = Kind::GlobalObject;
        ref.globalName_ = std::move(name);
        return ref;
    }

    Kind kind() const { return kind_; }
    const ResolvedLocalBinding *localBinding() const { return localBinding_; }
    const std::string &globalName() const { return globalName_; }
};

class ResolvedFunction {
    const AstFuncDecl *decl_ = nullptr;
    const AstNode *body_ = nullptr;
    std::string functionName_;
    std::string methodParentTypeName_;
    location loc_;
    bool topLevelEntry_ = false;
    bool guaranteedReturn_ = false;

    std::vector<const ResolvedLocalBinding *> params_;
    const ResolvedLocalBinding *selfBinding_ = nullptr;
    std::unordered_map<const AstVarDef *, const ResolvedLocalBinding *> variables_;
    std::unordered_map<const AstField *, ResolvedValueRef> fields_;
    std::unordered_map<const AstFuncRef *, std::string> functionRefs_;

public:
    ResolvedFunction(const AstFuncDecl *decl, const AstNode *body,
                     std::string functionName, std::string methodParentTypeName,
                     const location &loc,
                     bool topLevelEntry, bool guaranteedReturn)
        : decl_(decl),
          body_(body),
          functionName_(std::move(functionName)),
          methodParentTypeName_(std::move(methodParentTypeName)),
          loc_(loc),
          topLevelEntry_(topLevelEntry),
          guaranteedReturn_(guaranteedReturn) {}

    const AstFuncDecl *decl() const { return decl_; }
    const AstNode *body() const { return body_; }
    bool hasDeclaredFunction() const { return !functionName_.empty(); }
    const std::string &functionName() const { return functionName_; }
    bool isMethod() const { return !methodParentTypeName_.empty(); }
    const std::string &methodParentTypeName() const { return methodParentTypeName_; }
    const location &loc() const { return loc_; }
    bool isTopLevelEntry() const { return topLevelEntry_; }
    bool guaranteedReturn() const { return guaranteedReturn_; }

    void addParam(const ResolvedLocalBinding *binding) { params_.push_back(binding); }
    const std::vector<const ResolvedLocalBinding *> &params() const { return params_; }

    bool hasSelfBinding() const { return selfBinding_ != nullptr; }
    void setSelfBinding(const ResolvedLocalBinding *binding) { selfBinding_ = binding; }
    const ResolvedLocalBinding *selfBinding() const { return selfBinding_; }

    void bindVariable(const AstVarDef *node, const ResolvedLocalBinding *binding) {
        variables_[node] = binding;
    }
    const ResolvedLocalBinding *variable(const AstVarDef *node) const;

    void bindField(const AstField *node, ResolvedValueRef binding) {
        fields_[node] = binding;
    }
    const ResolvedValueRef *field(const AstField *node) const;

    void bindFunctionRef(const AstFuncRef *node, std::string functionName) {
        functionRefs_[node] = std::move(functionName);
    }
    const std::string *functionRef(const AstFuncRef *node) const;
};

class ResolvedModule {
    std::vector<std::unique_ptr<ResolvedLocalBinding>> localBindings_;
    std::vector<std::unique_ptr<ResolvedFunction>> functions_;

public:
    const ResolvedLocalBinding *createLocalBinding(ResolvedLocalBinding::Kind kind,
                                                   std::string name,
                                                   const AstNode *node,
                                                   const location &loc);

    ResolvedFunction *createFunction(const AstFuncDecl *decl, const AstNode *body,
                                     std::string functionName,
                                     std::string methodParentTypeName,
                                     const location &loc, bool topLevelEntry,
                                     bool guaranteedReturn);

    const std::vector<std::unique_ptr<ResolvedFunction>> &functions() const {
        return functions_;
    }
};

std::unique_ptr<ResolvedModule> resolveModule(GlobalScope *global, AstNode *root,
                                              const CompilationUnit *unit = nullptr);

}  // namespace lona
