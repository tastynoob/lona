#pragma once

#include "../ast/astnode.hh"
#include "../visitor.hh"
#include "../obj/value.hh"
#include <cassert>
#include <cstddef>
#include <llvm-18/llvm/ADT/ArrayRef.h>
#include <llvm-18/llvm/ADT/StringMap.h>
#include <llvm-18/llvm/IR/LLVMContext.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/Type.h>
#include <llvm-18/llvm/Support/Casting.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include <string>
#include <vector>

namespace lona {

class TypeClass;
class Object;
class PointerType;
class StructType;

static const int RVO_THRESHOLD = 16;

class TypeClass {
public:
    llvm::Type *const llvmType;
    std::string const full_name;
    int typeSize;  // the size of the type in bytes

    TypeClass(llvm::Type *llvmType, std::string full_name, int typeSize)
        : llvmType(llvmType), full_name(full_name), typeSize(typeSize) {}

    template<typename T>
    T *as() {
        return dynamic_cast<T *>(this);
    }

    bool equal(TypeClass *t) { return this == t; }

    bool shouldReturnByPointer() {
        if (as<StructType>()) {
            return typeSize > RVO_THRESHOLD;
        }
        return false;
    }

    virtual ObjectPtr newObj(uint32_t specifiers = Object::EMPTY);

    virtual void binaryOperation(llvm::IRBuilder<> &builder, ObjectPtr left,
                                 token_type op, ObjectPtr right, ObjectPtr& res) {}

    virtual void unaryOperation(llvm::IRBuilder<> &builder, token_type op,
                                ObjectPtr value, ObjectPtr& res) {}

    virtual void assignOperation(llvm::IRBuilder<> &builder, ObjectPtr left,
                                 ObjectPtr right, ObjectPtr& res) {}

    virtual void callOperation(Scope *scope, ObjectPtr value,
                               std::vector<ObjectPtr> args, ObjectPtr& res) {}

    virtual void fieldSelect(llvm::IRBuilder<> &builder, ObjectPtr value,
                             const std::string &field, ObjectPtr& res) {}
};

class BaseType : public TypeClass {
public:
    enum Type { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, BOOL } type;
    BaseType(llvm::Type *llvmType, Type type, std::string full_name,
             int typeSize)
        : TypeClass(llvmType, full_name, typeSize), type(type) {}
};

class StructType : public TypeClass {
public:
    // type : index
    using ValueTy = std::pair<TypeClass *, int>;

private:
    llvm::StringMap<ValueTy> members;
    llvm::StringMap<Function *> funcs;

    bool opaque = false;
public:
    StructType(llvm::Type *llvmType,
               llvm::StringMap<ValueTy> &&members,
               std::string full_name, int typeSize)
        : TypeClass(llvmType, full_name, typeSize), members(members), opaque(false) {}

    // create opaque struct
    StructType(llvm::Type *llvmType, std::string full_name)
        : TypeClass(llvmType, full_name, 0), opaque(true) {}

    TypeClass *getMember(const std::string &name) {
        auto it = members.find(name);
        if (it == members.end()) {
            return nullptr;
        }
        return it->second.first;
    }

    Function *getFunc(const std::string &name) {
        auto it = funcs.find(name);
        if (it == funcs.end()) {
            return nullptr;
        }
        return it->second;
    }

    void complete(llvm::StringMap<ValueTy>& members, int typeSize) {
        if (!opaque) { return; }

        this->typeSize = typeSize;

        this->members = std::move(members);

        if (auto* st = llvm::dyn_cast<llvm::StructType>(llvmType)) {
            assert(st->isOpaque());
            std::vector<llvm::Type*> body;
            for (auto& it : this->members) {
                body.push_back(it.second.first->llvmType);
            }
            st->setBody(body);
        }
    }

    void addFunc(std::string name, Function *func) {
        this->funcs.insert({name, func});
    }

    bool isOpaque() const { return opaque; }

    ObjectPtr newObj(uint32_t specifiers = Object::EMPTY) override;

    void fieldSelect(llvm::IRBuilder<> &builder, ObjectPtr value,
                     const std::string &field, ObjectPtr& ret) override;
};

class FuncType : public TypeClass {
    std::vector<TypeClass *> argTypes;
    TypeClass * retType = nullptr;
    bool hasSROA = false;
public:
    bool SROA() const { return hasSROA; }
    auto& getArgTypes() const { return argTypes; }
    TypeClass *getRetType() const { return retType; }

    FuncType(llvm::Type *llvmType, std::vector<TypeClass *> &&args,
             TypeClass *retType, std::string full_name, int typeSize)
        : TypeClass(llvmType, full_name, typeSize),
          argTypes(args),
          retType(retType),
          hasSROA(retType->shouldReturnByPointer()) {
        // if hasSroa, the first arg is the return value
    }

    ObjectPtr newObj(uint32_t specifiers = Object::EMPTY) override {
        assert(false);
    }

    void callOperation(Scope *scope, ObjectPtr value,
                       std::vector<ObjectPtr> args, ObjectPtr& res) override;
};

class PointerType : public TypeClass {
    TypeClass *pointeeType;

public:
    PointerType(TypeClass *pointeeType)
        : TypeClass(pointeeType->llvmType->getPointerTo(),
                    pointeeType->full_name + "*", 8),
          pointeeType(pointeeType) {
        assert(!pointeeType->llvmType->isPointerTy());
    }
    TypeClass *getPointeeType() { return pointeeType; }
};


class TypeManager {
    llvm::Module& module;

    llvm::StringMap<TypeClass*> typeMap;
public:
    TypeManager(llvm::Module& module) : module(module) {
    }

    llvm::LLVMContext& getContext() { return module.getContext(); }
    llvm::Module& getModule() { return module; }

    bool addType(llvm::StringRef name, TypeClass *type) {
        if (typeMap.find(name) != typeMap.end()) {
            return false;
        }
        typeMap[name] = type;
        return true;
    }

    TypeClass *getType(llvm::StringRef name) {
        auto it = typeMap.find(name);
        if (it == typeMap.end()) {
            return nullptr;
        }
        return it->second;
    }

    TypeClass *getType(TypeNode* typeNode);

    TypeClass *createPointerType(TypeClass *pointeeType) {
        std::string full_name = pointeeType->full_name + "*";
        auto it = typeMap.find(full_name);
        if (it != typeMap.end()) {
            return it->second;
        }

        auto ptrType = new PointerType(pointeeType);
        addType(full_name, ptrType);
        return ptrType;
    }
};

}  // namespace lona