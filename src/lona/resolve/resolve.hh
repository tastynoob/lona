#pragma once

#include "lona/ast/astnode.hh"
#include "lona/module/module_interface.hh"
#include "lona/sema/entity.hh"
#include "lona/type/type.hh"
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
    BindingKind bindingKind_ = BindingKind::Value;
    string name_;
    const AstNode *node_ = nullptr;
    location loc_;

public:
    ResolvedLocalBinding(Kind kind, BindingKind bindingKind, string name,
                         const AstNode *node, const location &loc)
        : kind_(kind),
          bindingKind_(bindingKind),
          name_(std::move(name)),
          node_(node),
          loc_(loc) {}

    Kind kind() const { return kind_; }
    BindingKind bindingKind() const { return bindingKind_; }
    bool isRefBinding() const { return bindingKind_ == BindingKind::Ref; }
    const string &name() const { return name_; }
    const AstNode *node() const { return node_; }
    const location &loc() const { return loc_; }

    const AstVarDecl *parameterDecl() const {
        return dynamic_cast<const AstVarDecl *>(node_);
    }

    const AstVarDef *variableDecl() const {
        return dynamic_cast<const AstVarDef *>(node_);
    }
};

class ResolvedEntityRef {
public:
    enum class Kind {
        Invalid,
        LocalBinding,
        InlineGlobal,
        GlobalValue,
        GenericFunction,
        Type,
        GenericType,
        Trait,
        Module,
    };

private:
    Kind kind_ = Kind::Invalid;
    const ResolvedLocalBinding *localBinding_ = nullptr;
    const AstVarDef *inlineDecl_ = nullptr;
    const ModuleInterface::FunctionDecl *functionDecl_ = nullptr;
    const ModuleInterface::TypeDecl *typeDecl_ = nullptr;
    const ModuleInterface *ownerInterface_ = nullptr;
    const CompilationUnit *ownerUnit_ = nullptr;
    string resolvedName_;

public:
    static ResolvedEntityRef invalid() { return ResolvedEntityRef(); }

    static ResolvedEntityRef local(const ResolvedLocalBinding *binding) {
        ResolvedEntityRef ref;
        ref.kind_ = Kind::LocalBinding;
        ref.localBinding_ = binding;
        return ref;
    }

    static ResolvedEntityRef inlineGlobal(
        string name, const AstVarDef *inlineDecl,
        const CompilationUnit *ownerUnit = nullptr) {
        ResolvedEntityRef ref;
        ref.kind_ = Kind::InlineGlobal;
        ref.inlineDecl_ = inlineDecl;
        ref.ownerUnit_ = ownerUnit;
        ref.resolvedName_ = std::move(name);
        return ref;
    }

    static ResolvedEntityRef globalValue(string name) {
        ResolvedEntityRef ref;
        ref.kind_ = Kind::GlobalValue;
        ref.resolvedName_ = std::move(name);
        return ref;
    }

    static ResolvedEntityRef genericFunction(
        string name, const ModuleInterface::FunctionDecl *functionDecl,
        const ModuleInterface *ownerInterface = nullptr) {
        ResolvedEntityRef ref;
        ref.kind_ = Kind::GenericFunction;
        ref.resolvedName_ = std::move(name);
        ref.functionDecl_ = functionDecl;
        ref.ownerInterface_ = ownerInterface;
        return ref;
    }

    static ResolvedEntityRef type(string name) {
        ResolvedEntityRef ref;
        ref.kind_ = Kind::Type;
        ref.resolvedName_ = std::move(name);
        return ref;
    }

    static ResolvedEntityRef genericType(string name,
                                         const ModuleInterface::TypeDecl *typeDecl,
                                         const ModuleInterface *ownerInterface = nullptr) {
        ResolvedEntityRef ref;
        ref.kind_ = Kind::GenericType;
        ref.resolvedName_ = std::move(name);
        ref.typeDecl_ = typeDecl;
        ref.ownerInterface_ = ownerInterface;
        return ref;
    }

    static ResolvedEntityRef trait(string name) {
        ResolvedEntityRef ref;
        ref.kind_ = Kind::Trait;
        ref.resolvedName_ = std::move(name);
        return ref;
    }

    static ResolvedEntityRef module(string name) {
        ResolvedEntityRef ref;
        ref.kind_ = Kind::Module;
        ref.resolvedName_ = std::move(name);
        return ref;
    }

    Kind kind() const { return kind_; }
    bool valid() const { return kind_ != Kind::Invalid; }
    const ResolvedLocalBinding *localBinding() const { return localBinding_; }
    const AstVarDef *inlineDecl() const { return inlineDecl_; }
    const ModuleInterface::FunctionDecl *functionDecl() const {
        return functionDecl_;
    }
    const ModuleInterface::TypeDecl *typeDecl() const { return typeDecl_; }
    const ModuleInterface *ownerInterface() const { return ownerInterface_; }
    const CompilationUnit *ownerUnit() const { return ownerUnit_; }
    const string &resolvedName() const { return resolvedName_; }
};

class ResolvedFunction {
    const AstFuncDecl *decl_ = nullptr;
    const AstNode *body_ = nullptr;
    string functionName_;
    string methodParentTypeName_;
    location loc_;
    bool topLevelEntry_ = false;
    bool languageEntry_ = false;
    bool guaranteedReturn_ = false;
    bool templateValidationOnly_ = false;
    std::vector<string> genericTypeParams_;
    std::unordered_map<std::string, std::string> genericTypeParamBounds_;
    const ModuleInterface *genericOwnerInterface_ = nullptr;
    std::unordered_map<std::string, TypeClass *> concreteGenericTypes_;

    std::vector<const ResolvedLocalBinding *> params_;
    const ResolvedLocalBinding *selfBinding_ = nullptr;
    std::unordered_map<const AstVarDef *, const ResolvedLocalBinding *>
        variables_;
    std::unordered_map<const AstField *, ResolvedEntityRef> fields_;
    std::unordered_map<const AstDotLike *, ResolvedEntityRef> dotLikes_;
    std::unordered_map<const AstFuncRef *, ResolvedEntityRef> functionRefs_;

public:
    ResolvedFunction(const AstFuncDecl *decl, const AstNode *body,
                     string functionName, string methodParentTypeName,
                     const location &loc, bool topLevelEntry,
                     bool languageEntry, bool guaranteedReturn,
                     bool templateValidationOnly = false,
                     std::vector<string> genericTypeParams = {},
                     std::unordered_map<std::string, std::string>
                         genericTypeParamBounds = {},
                     const ModuleInterface *genericOwnerInterface = nullptr,
                     std::unordered_map<std::string, TypeClass *>
                         concreteGenericTypes = {})
        : decl_(decl),
          body_(body),
          functionName_(std::move(functionName)),
          methodParentTypeName_(std::move(methodParentTypeName)),
          loc_(loc),
          topLevelEntry_(topLevelEntry),
          languageEntry_(languageEntry),
          guaranteedReturn_(guaranteedReturn),
          templateValidationOnly_(templateValidationOnly),
          genericTypeParams_(std::move(genericTypeParams)),
          genericTypeParamBounds_(std::move(genericTypeParamBounds)),
          genericOwnerInterface_(genericOwnerInterface),
          concreteGenericTypes_(std::move(concreteGenericTypes)) {}

    const AstFuncDecl *decl() const { return decl_; }
    const AstNode *body() const { return body_; }
    bool hasDeclaredFunction() const { return !functionName_.empty(); }
    const string &functionName() const { return functionName_; }
    bool isMethod() const { return !methodParentTypeName_.empty(); }
    const string &methodParentTypeName() const { return methodParentTypeName_; }
    const location &loc() const { return loc_; }
    bool isTopLevelEntry() const { return topLevelEntry_; }
    bool isLanguageEntry() const { return languageEntry_; }
    bool guaranteedReturn() const { return guaranteedReturn_; }
    bool isTemplateValidationOnly() const { return templateValidationOnly_; }
    const std::vector<string> &genericTypeParams() const {
        return genericTypeParams_;
    }
    const std::unordered_map<std::string, std::string> &
    genericTypeParamBounds() const {
        return genericTypeParamBounds_;
    }
    const std::string *genericTypeParamBound(const std::string &name) const {
        if (auto found = genericTypeParamBounds_.find(name);
            found != genericTypeParamBounds_.end() && !found->second.empty()) {
            return &found->second;
        }
        return nullptr;
    }
    bool genericTypeParamHasBound(const std::string &name) const {
        return genericTypeParamBound(name) != nullptr;
    }
    const ModuleInterface *genericOwnerInterface() const {
        return genericOwnerInterface_;
    }
    const std::unordered_map<std::string, TypeClass *> &
    concreteGenericTypes() const {
        return concreteGenericTypes_;
    }
    TypeClass *concreteGenericType(const std::string &name) const {
        if (auto found = concreteGenericTypes_.find(name);
            found != concreteGenericTypes_.end()) {
            return found->second;
        }
        return nullptr;
    }

    void addParam(const ResolvedLocalBinding *binding) {
        params_.push_back(binding);
    }
    const std::vector<const ResolvedLocalBinding *> &params() const {
        return params_;
    }

    bool hasSelfBinding() const { return selfBinding_ != nullptr; }
    void setSelfBinding(const ResolvedLocalBinding *binding) {
        selfBinding_ = binding;
    }
    const ResolvedLocalBinding *selfBinding() const { return selfBinding_; }

    void bindVariable(const AstVarDef *node,
                      const ResolvedLocalBinding *binding) {
        variables_[node] = binding;
    }
    const ResolvedLocalBinding *variable(const AstVarDef *node) const;

    void bindField(const AstField *node, ResolvedEntityRef binding) {
        fields_[node] = binding;
    }
    const ResolvedEntityRef *field(const AstField *node) const;

    void bindDotLike(const AstDotLike *node, ResolvedEntityRef binding) {
        dotLikes_[node] = std::move(binding);
    }
    const ResolvedEntityRef *dotLike(const AstDotLike *node) const;

    void bindFunctionRef(const AstFuncRef *node, ResolvedEntityRef binding) {
        functionRefs_[node] = std::move(binding);
    }
    const ResolvedEntityRef *functionRef(const AstFuncRef *node) const;
};

class ResolvedModule {
    std::vector<std::unique_ptr<ResolvedLocalBinding>> localBindings_;
    std::vector<std::unique_ptr<ResolvedFunction>> functions_;

public:
    const ResolvedLocalBinding *createLocalBinding(
        ResolvedLocalBinding::Kind kind, BindingKind bindingKind, string name,
        const AstNode *node, const location &loc);

    ResolvedFunction *createFunction(const AstFuncDecl *decl,
                                     const AstNode *body, string functionName,
                                     string methodParentTypeName,
                                     const location &loc, bool topLevelEntry,
                                     bool languageEntry,
                                     bool guaranteedReturn,
                                     bool templateValidationOnly = false,
                                     std::vector<string> genericTypeParams = {},
                                     std::unordered_map<std::string, std::string>
                                         genericTypeParamBounds = {},
                                     const ModuleInterface *genericOwnerInterface = nullptr,
                                     std::unordered_map<std::string, TypeClass *>
                                         concreteGenericTypes = {});

    const std::vector<std::unique_ptr<ResolvedFunction>> &functions() const {
        return functions_;
    }
};

std::unique_ptr<ResolvedModule>
resolveModule(GlobalScope *global, AstNode *root,
              const CompilationUnit *unit = nullptr, bool rootModule = false);

std::unique_ptr<ResolvedModule>
resolveGenericFunctionInstance(
    GlobalScope *global, const CompilationUnit *unit, const AstFuncDecl *decl,
    string resolvedFunctionName,
    const ModuleInterface *genericOwnerInterface,
    std::unordered_map<std::string, TypeClass *> concreteGenericTypes);

std::unique_ptr<ResolvedModule>
resolveGenericMethodInstance(
    GlobalScope *global, const CompilationUnit *unit, const AstFuncDecl *decl,
    string resolvedFunctionName, string methodParentTypeName,
    std::vector<string> genericTypeParams,
    std::unordered_map<std::string, std::string> genericTypeParamBounds,
    const ModuleInterface *genericOwnerInterface,
    std::unordered_map<std::string, TypeClass *> concreteGenericTypes);

}  // namespace lona
