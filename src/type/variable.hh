#pragma once

#include "llvm.hh"

namespace lona {

class TypeClass;

class BaseVariable {
protected:
    TypeClass *type;
    llvm::Value *val;
    uint32_t specifiers;

public:
    enum Specifier : uint32_t {
        CONST = 1 << 0,  // Literal constants
    };
    BaseVariable(BaseVariable *other, TypeClass *cast_type);
    BaseVariable(llvm::Value *val, TypeClass *type, uint64_t specifiers = 0)
        : val(val), type(type), specifiers(specifiers) {}
    TypeClass *getType() { return type; }
    bool isConst() { return specifiers & CONST; }
    bool isVariable() { return !isConst(); }

    llvm::Value *read(llvm::IRBuilder<> &builder);
    void write(llvm::IRBuilder<> &builder, BaseVariable *src);
    // cast src'type to this
    BaseVariable *castToSelf(BaseVariable *src);
};

class VariableManger {
    llvm::IRBuilder<> &builder;
    std::vector<llvm::StringMap<BaseVariable *>> variables;

public:
    VariableManger(llvm::IRBuilder<> &builder) : builder(builder) {}
    void enterScope() {
        variables.push_back(llvm::StringMap<BaseVariable *>());
    }
    void leaveScope() { variables.pop_back(); }
    void addVariable(llvm::StringRef name, BaseVariable *var) {
        assert(!variables.empty());
        variables.back()[name] = var;
    }
    BaseVariable *getVariable(llvm::StringRef name);
};

}  // namespace lona