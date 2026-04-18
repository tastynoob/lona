#pragma once

#include "lona/ast/astnode.hh"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lona {

class Object;
class TypeClass;
class FuncType;
class ResolvedLocalBinding;

enum class EntityKind {
    Invalid,
    LocalBinding,
    Object,
    TypedValue,
    Type,
    ConstructorSet,
};

struct CallArgSpec {
    AstNode *syntax = nullptr;
    AstNode *value = nullptr;
    BindingKind bindingKind = BindingKind::Value;
    std::optional<std::string> name;
    location loc;

    bool isNamed() const { return name.has_value(); }
    bool isRef() const { return bindingKind == BindingKind::Ref; }
};

using CallArgList = std::vector<CallArgSpec>;

enum class LookupResultKind {
    Invalid,
    Deferred,
    NotFound,
    Ambiguous,
    ValueField,
    Method,
    ExtensionMethod,
    TypeMember,
    ConstructorSet,
    InjectedMember,
};

enum class CallResolutionKind {
    Invalid,
    Deferred,
    FunctionCall,
    FunctionPointerCall,
    ConstructorCall,
    ArrayIndex,
    NotCallable,
    Ambiguous,
};

struct LookupResult;
struct CallResolution;

class EntityRef {
    EntityKind kind_ = EntityKind::Invalid;
    const ResolvedLocalBinding *localBinding_ = nullptr;
    Object *object_ = nullptr;
    TypeClass *type_ = nullptr;

    explicit EntityRef(EntityKind kind) : kind_(kind) {}

public:
    EntityRef() = default;

    static EntityRef invalid() { return EntityRef(); }

    static EntityRef localBinding(const ResolvedLocalBinding *binding) {
        EntityRef ref(EntityKind::LocalBinding);
        ref.localBinding_ = binding;
        return ref;
    }

    static EntityRef object(Object *object) {
        EntityRef ref(EntityKind::Object);
        ref.object_ = object;
        return ref;
    }

    static EntityRef typedValue(TypeClass *type) {
        EntityRef ref(EntityKind::TypedValue);
        ref.type_ = type;
        return ref;
    }

    static EntityRef type(TypeClass *type) {
        EntityRef ref(EntityKind::Type);
        ref.type_ = type;
        return ref;
    }

    static EntityRef constructorSet(TypeClass *type) {
        EntityRef ref(EntityKind::ConstructorSet);
        ref.type_ = type;
        return ref;
    }

    EntityKind kind() const { return kind_; }
    bool valid() const { return kind_ != EntityKind::Invalid; }
    bool isValueLike() const {
        return kind_ == EntityKind::Object || kind_ == EntityKind::TypedValue;
    }

    const ResolvedLocalBinding *asLocalBinding() const { return localBinding_; }
    Object *asObject() const { return object_; }
    TypeClass *asType() const {
        if (kind_ == EntityKind::Type || kind_ == EntityKind::ConstructorSet) {
            return type_;
        }
        return nullptr;
    }
    TypeClass *valueType() const {
        if (kind_ == EntityKind::Object) {
            return object_ ? object_->getType() : nullptr;
        }
        if (kind_ == EntityKind::TypedValue) {
            return type_;
        }
        return nullptr;
    }

    LookupResult dot(std::string member) const;
    CallResolution applyCall(CallArgList callArgs) const;
};

struct LookupResult {
    LookupResultKind kind = LookupResultKind::Invalid;
    EntityRef owner;
    EntityRef resultEntity;
    std::string member;

    bool valid() const { return kind != LookupResultKind::Invalid; }
    bool isDeferred() const { return kind == LookupResultKind::Deferred; }
    bool hasResultEntity() const { return resultEntity.valid(); }

    static LookupResult deferred(EntityRef owner, std::string member) {
        LookupResult result;
        result.kind = LookupResultKind::Deferred;
        result.owner = owner;
        result.member = std::move(member);
        return result;
    }
};

struct CallResolution {
    CallResolutionKind kind = CallResolutionKind::Invalid;
    EntityRef callee;
    EntityRef resultEntity;
    CallArgList args;
    FuncType *callType = nullptr;
    const std::vector<string> *paramNames = nullptr;
    std::size_t argOffset = 0;

    bool valid() const { return kind != CallResolutionKind::Invalid; }
    bool isDeferred() const { return kind == CallResolutionKind::Deferred; }
    bool hasResultEntity() const { return resultEntity.valid(); }

    static CallResolution deferred(EntityRef callee, CallArgList args) {
        CallResolution resolution;
        resolution.kind = CallResolutionKind::Deferred;
        resolution.callee = callee;
        resolution.args = std::move(args);
        return resolution;
    }
};

inline LookupResult
EntityRef::dot(std::string member) const {
    return LookupResult::deferred(*this, std::move(member));
}

inline CallResolution
EntityRef::applyCall(CallArgList callArgs) const {
    return CallResolution::deferred(*this, std::move(callArgs));
}

}  // namespace lona
