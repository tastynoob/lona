#include "lona/abi/abi.hh"
#include "lona/abi/native_abi.hh"
#include "lona/ast/array_dim.hh"
#include "lona/ast/astnode.hh"
#include "lona/declare/support.hh"
#include "lona/emit/debug.hh"
#include "lona/err/err.hh"
#include "lona/module/module_graph.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/calls.hh"
#include "lona/sema/hir.hh"
#include "lona/sema/moduleentry.hh"
#include "lona/sym/func.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include "lona/visitor.hh"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <llvm-18/llvm/IR/BasicBlock.h>
#include <llvm-18/llvm/IR/Constants.h>
#include <llvm-18/llvm/IR/Function.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/Type.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lona {
namespace llvmcodegen_impl {

FuncType *
getOrCreateModuleEntryType(TypeTable *typeMgr) {
    return typeMgr->getOrCreateFunctionType({}, i32Ty);
}

llvm::Function *
getOrCreateModuleInitDeclaration(GlobalScope *global, TypeTable *typeMgr,
                                 const CompilationUnit &unit) {
    auto symbolName = moduleInitEntrySymbolName(unit);
    if (auto *existing = global->module.getFunction(symbolName)) {
        return existing;
    }

    auto *func = llvm::Function::Create(
        typeMgr->getLLVMFunctionType(getOrCreateModuleEntryType(typeMgr)),
        llvm::Function::ExternalLinkage, llvm::Twine(symbolName),
        global->module);
    annotateFunctionAbi(*func, AbiKind::Native);
    return func;
}

llvm::GlobalVariable *
getOrCreateModuleInitState(GlobalScope *global, const CompilationUnit &unit) {
    auto symbolName = moduleInitStateSymbolName(unit);
    if (auto *existing = global->module.getGlobalVariable(symbolName)) {
        return existing;
    }

    auto *i32Type = llvm::Type::getInt32Ty(global->module.getContext());
    return new llvm::GlobalVariable(
        global->module, i32Type, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(i32Type, 0), llvm::Twine(symbolName));
}

llvm::GlobalVariable *
getOrCreateModuleInitResult(GlobalScope *global, const CompilationUnit &unit) {
    auto symbolName = moduleInitResultSymbolName(unit);
    if (auto *existing = global->module.getGlobalVariable(symbolName)) {
        return existing;
    }

    auto *i32Type = llvm::Type::getInt32Ty(global->module.getContext());
    return new llvm::GlobalVariable(
        global->module, i32Type, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(i32Type, 0), llvm::Twine(symbolName));
}

std::string
traitWitnessSymbolName(llvm::StringRef traitName, llvm::StringRef selfTypeName) {
    return "__lona_trait_witness__" +
           mangleModuleEntryComponent(traitName) + "__" +
           mangleModuleEntryComponent(selfTypeName);
}

using ByteStringGlobalCache =
    std::unordered_map<struct ByteStringGlobalKey, llvm::GlobalVariable *,
                       struct ByteStringGlobalKeyHash>;

struct ByteStringGlobalKey {
    std::string bytes;
    std::string sourcePath;
    int beginLine = 0;
    int beginColumn = 0;
    int endLine = 0;
    int endColumn = 0;

    bool operator==(const ByteStringGlobalKey &other) const {
        return bytes == other.bytes && sourcePath == other.sourcePath &&
               beginLine == other.beginLine &&
               beginColumn == other.beginColumn && endLine == other.endLine &&
               endColumn == other.endColumn;
    }
};

struct ByteStringGlobalKeyHash {
    std::size_t operator()(const ByteStringGlobalKey &key) const {
        auto hashCombine = [](std::size_t seed, std::size_t value) {
            return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
        };

        std::size_t seed = std::hash<std::string>{}(key.bytes);
        seed = hashCombine(seed, std::hash<std::string>{}(key.sourcePath));
        seed = hashCombine(seed, std::hash<int>{}(key.beginLine));
        seed = hashCombine(seed, std::hash<int>{}(key.beginColumn));
        seed = hashCombine(seed, std::hash<int>{}(key.endLine));
        seed = hashCombine(seed, std::hash<int>{}(key.endColumn));
        return seed;
    }
};

class FunctionCompiler {
    struct LoopContext {
        llvm::BasicBlock *continueBlock = nullptr;
        llvm::BasicBlock *breakBlock = nullptr;
    };

    TypeTable *typeMgr;
    GlobalScope *global;
    Scope *scope;
    FuncScope *funcScope;
    llvm::LLVMContext &context;
    DebugInfoContext *debug;
    const CompilationUnit *unit;
    const ModuleGraph *moduleGraph;
    llvm::DISubprogram *debugSubprogram = nullptr;
    AbiFunctionSignature abiSignature;
    bool returnByPointer = false;
    llvm::GlobalVariable *moduleInitState = nullptr;
    llvm::GlobalVariable *moduleInitResult = nullptr;
    location currentLocation;
    bool hasCurrentLocation = false;
    std::vector<LoopContext> loopStack;
    ByteStringGlobalCache &byteStringGlobals_;

    [[noreturn]] void error(const std::string &message) {
        if (hasCurrentLocation) {
            lona::error(currentLocation, message);
        }
        lona::error(message);
    }

    [[noreturn]] void error(const std::string &message,
                            const std::string &hint) {
        if (hasCurrentLocation) {
            lona::error(currentLocation, message, hint);
        }
        throw DiagnosticError(DiagnosticError::Category::Semantic, message,
                              hint);
    }

    [[noreturn]] void error(const location &loc, const std::string &message,
                            const std::string &hint = std::string()) {
        lona::error(loc, message, hint);
    }

    [[noreturn]] void functionError(HIRFunc *hirFunc,
                                    const std::string &message,
                                    const std::string &hint = std::string()) {
        if (hirFunc) {
            error(hirFunc->getLocation(), message, hint);
        }
        error(message, hint);
    }

    llvm::Constant *buildByteStringArrayConstant(const ::string &bytes) {
        std::vector<std::uint8_t> data;
        data.reserve(bytes.size() + 1);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            data.push_back(static_cast<std::uint8_t>(
                static_cast<unsigned char>(bytes[i])));
        }
        data.push_back(0);
        return llvm::ConstantDataArray::get(context, data);
    }

    std::string nextByteStringGlobalName() {
        static std::uint64_t nextId = 0;
        return ".lona.bytes." + std::to_string(nextId++);
    }

    llvm::GlobalVariable *createByteStringGlobal(const ::string &bytes) {
        auto *initializer = buildByteStringArrayConstant(bytes);
        auto *llvmArrayType =
            llvm::cast<llvm::ArrayType>(initializer->getType());
        auto *globalValue =
            new llvm::GlobalVariable(scope->module, llvmArrayType, true,
                                     llvm::GlobalValue::PrivateLinkage,
                                     initializer, nextByteStringGlobalName());
        globalValue->setAlignment(llvm::MaybeAlign(1));
        return globalValue;
    }

    ByteStringGlobalKey
    byteStringGlobalKey(const HIRByteStringLiteral *byteString) const {
        ByteStringGlobalKey key;
        if (!byteString) {
            return key;
        }

        key.bytes = toStdString(byteString->getBytes());
        const auto &loc = byteString->getLocation();
        if (loc.begin.filename) {
            key.sourcePath = *loc.begin.filename;
        }
        key.beginLine = loc.begin.line;
        key.beginColumn = loc.begin.column;
        key.endLine = loc.end.line;
        key.endColumn = loc.end.column;
        return key;
    }

    llvm::GlobalVariable *
    getOrCreateByteStringGlobal(const HIRByteStringLiteral *byteString) {
        if (!byteString) {
            return nullptr;
        }
        auto key = byteStringGlobalKey(byteString);
        auto found = byteStringGlobals_.find(key);
        if (found != byteStringGlobals_.end()) {
            return found->second;
        }
        auto *globalValue = createByteStringGlobal(byteString->getBytes());
        byteStringGlobals_.emplace(std::move(key), globalValue);
        return globalValue;
    }

    const ModuleInterface::TraitDecl *requireVisibleTraitDecl(
        llvm::StringRef resolvedName, const location &loc,
        const std::string &contextName) {
        if (!unit) {
            error(loc,
                  contextName +
                      " requires workspace trait metadata during LLVM lowering",
                  "This looks like a compiler pipeline bug.");
        }
        auto *traitDecl = unit->findVisibleTraitByResolvedName(
            string(resolvedName.str()));
        if (!traitDecl) {
            error(loc,
                  "unknown trait `" + resolvedName.str() + "` during " +
                      contextName,
                  "This looks like a compiler pipeline bug.");
        }
        return traitDecl;
    }

    std::string resolveConcreteTraitMethodLookupName(
        StructType *selfType, const ModuleInterface::TraitDecl &traitDecl,
        llvm::StringRef methodName) {
        if (!selfType) {
            return {};
        }
        auto traitMethodKey =
            traitMethodSlotKey(traitDecl.exportedName, methodName);
        if (selfType->getTraitMethodTypeByKey(toStringRef(traitMethodKey)) ||
            scope->getMethodFunction(selfType, toStringRef(traitMethodKey))) {
            return traitMethodKey;
        }
        if (selfType->getMethodType(methodName) ||
            scope->getMethodFunction(selfType, methodName)) {
            return methodName.str();
        }
        return {};
    }

    llvm::ArrayType *getTraitWitnessLLVMType(std::size_t slotCount) {
        auto *ptrType = llvm::PointerType::getUnqual(context);
        return llvm::ArrayType::get(ptrType, slotCount);
    }

    llvm::GlobalVariable *getOrCreateTraitWitnessTable(
        const ModuleInterface::TraitDecl &traitDecl, StructType *selfType,
        const location &loc) {
        if (!selfType) {
            error(loc,
                  "trait witness table lowering requires a concrete struct type",
                  "This looks like a compiler pipeline bug.");
        }

        auto symbolName = traitWitnessSymbolName(
            toStringRef(traitDecl.exportedName), toStringRef(selfType->full_name));
        if (auto *existing = global->module.getGlobalVariable(symbolName)) {
            return existing;
        }

        std::vector<llvm::Constant *> slots;
        slots.reserve(traitDecl.methods.size());
        auto *ptrType = llvm::PointerType::getUnqual(context);
        for (const auto &method : traitDecl.methods) {
            auto methodLookupName = resolveConcreteTraitMethodLookupName(
                selfType, traitDecl, toStringRef(method.localName));
            auto *callee = methodLookupName.empty()
                               ? nullptr
                               : scope->getMethodFunction(
                                     selfType, toStringRef(methodLookupName));
            if (!callee || !callee->getllvmValue()) {
                error(loc,
                      "missing trait method `" +
                          toStdString(traitDecl.exportedName) + "." +
                          toStdString(method.localName) + "` for `" +
                          toStdString(selfType->full_name) +
                          "` for trait witness lowering",
                      "This looks like a compiler pipeline bug.");
            }
            auto *calleeConstant =
                llvm::dyn_cast<llvm::Constant>(callee->getllvmValue());
            if (!calleeConstant) {
                error(loc,
                      "trait witness lowering expected a constant method symbol",
                      "This looks like a compiler pipeline bug.");
            }
            slots.push_back(llvm::ConstantExpr::getPointerCast(
                calleeConstant, ptrType));
        }

        auto *witnessType = getTraitWitnessLLVMType(traitDecl.methods.size());
        auto *initializer = llvm::ConstantArray::get(witnessType, slots);
        return new llvm::GlobalVariable(
            global->module, witnessType, true,
            llvm::GlobalValue::InternalLinkage, initializer,
            llvm::Twine(symbolName));
    }

    ObjectPtr materializeLocal(TypeClass *type, Object *initVal) {
        auto obj = type->newObj(Object::VARIABLE);
        obj->createllvmValue(scope);
        if (initVal != nullptr) {
            obj->set(scope, initVal);
        }
        return obj;
    }

    ObjectPtr emitTraitObjectCast(HIRTraitObjectCast *cast) {
        auto source = compileExpr(cast->getSource());
        if (!source) {
            error("trait object cast requires a source value");
        }

        auto *dynType = asUnqualified<DynTraitType>(cast->getType());
        if (!dynType) {
            error("trait object cast is missing its dyn trait type");
        }
        auto *selfType = asUnqualified<StructType>(source->getType());
        if (!selfType) {
            error("trait object cast requires a concrete struct source type");
        }
        if (!source->isVariable() || source->isRegVal() ||
            !source->getllvmValue()) {
            error("trait object cast requires an addressable source value");
        }

        const auto *traitDecl = requireVisibleTraitDecl(
            toStringRef(dynType->traitName()), cast->getLocation(),
            "trait object cast");
        auto *witness = getOrCreateTraitWitnessTable(*traitDecl, selfType,
                                                     cast->getLocation());
        auto *llvmDynType = scope->getLLVMType(cast->getType());
        auto *ptrType = llvm::PointerType::getUnqual(context);

        llvm::Value *aggregate = llvm::UndefValue::get(llvmDynType);
        auto *dataPtr = source->getllvmValue();
        if (dataPtr->getType() != ptrType) {
            dataPtr = scope->builder.CreatePointerCast(dataPtr, ptrType);
        }
        auto *witnessPtr = llvm::ConstantExpr::getPointerCast(witness, ptrType);
        aggregate = scope->builder.CreateInsertValue(aggregate, dataPtr, {0});
        aggregate =
            scope->builder.CreateInsertValue(aggregate, witnessPtr, {1});
        return makeReadonlyValue(cast->getType(), aggregate);
    }

    ObjectPtr emitTraitObjectCall(HIRTraitObjectCall *call) {
        auto receiver = compileExpr(call->getReceiver());
        if (!receiver) {
            error("trait object call requires a receiver value");
        }
        auto *slotFuncType = call->getSlotFuncType();
        if (!slotFuncType || slotFuncType->getArgTypes().empty()) {
            error("trait object call is missing its slot function type");
        }

        const auto *traitDecl = requireVisibleTraitDecl(
            toStringRef(call->getTraitName()), call->getLocation(),
            "trait object call");
        if (call->getSlotIndex() >= traitDecl->methods.size()) {
            error("trait object call slot index is out of range");
        }

        auto *aggregate = receiver->get(scope);
        auto *ptrType = llvm::PointerType::getUnqual(context);
        auto *dataPtr =
            scope->builder.CreateExtractValue(aggregate, {0}, "trait.data");
        auto *witnessPtr = scope->builder.CreateExtractValue(
            aggregate, {1}, "trait.witness");
        if (dataPtr->getType() != ptrType) {
            dataPtr = scope->builder.CreatePointerCast(dataPtr, ptrType);
        }
        if (witnessPtr->getType() != ptrType) {
            witnessPtr = scope->builder.CreatePointerCast(witnessPtr, ptrType);
        }

        auto *witnessType = getTraitWitnessLLVMType(traitDecl->methods.size());
        auto *zero = scope->builder.getInt32(0);
        auto *slotPtr = scope->builder.CreateInBoundsGEP(
            witnessType, witnessPtr,
            {zero, scope->builder.getInt32(call->getSlotIndex())},
            "trait.slot.ptr");
        auto *slotValue =
            scope->builder.CreateLoad(ptrType, slotPtr, "trait.slot");

        std::vector<ObjectPtr> args;
        args.reserve(1 + call->getArgs().size());
        args.push_back(
            makeReadonlyValue(slotFuncType->getArgTypes().front(), dataPtr));
        for (auto *argExpr : call->getArgs()) {
            auto arg = compileExpr(argExpr);
            if (!arg) {
                error("trait object call argument did not produce a value");
            }
            args.push_back(arg);
        }
        return emitFunctionCall(scope, slotValue, slotFuncType, args, true);
    }

    ObjectPtr materializeBinding(const ObjectPtr &obj, Object *initVal = nullptr) {
        if (!obj) {
            error("missing binding object");
        }
        if (obj->isRefAlias()) {
            if (initVal == nullptr) {
                error("reference binding requires an addressable source");
            }
            if (!initVal->isVariable() || initVal->isRegVal() ||
                !initVal->getllvmValue()) {
                error("reference binding expects an addressable source");
            }
            if (!isConstQualificationConvertible(obj->getType(),
                                                 initVal->getType())) {
                error("reference binding type mismatch during lowering");
            }
            obj->setllvmValue(initVal->getllvmValue());
            return obj;
        }
        obj->createllvmValue(scope);
        if (initVal != nullptr) {
            obj->set(scope, initVal);
        }
        return obj;
    }

    ObjectPtr materializeIndirectValueBinding(const ObjectPtr &obj,
                                              llvm::Value *incomingPtr) {
        if (!obj || !incomingPtr) {
            error("indirect value binding requires an incoming pointer");
        }
        auto bound = materializeBinding(obj);
        auto *value = scope->builder.CreateLoad(
            scope->getLLVMType(obj->getType()), incomingPtr);
        scope->builder.CreateStore(value, bound->getllvmValue());
        return bound;
    }

    ObjectPtr materializeDirectValueBinding(const ObjectPtr &obj,
                                            llvm::Value *incomingValue,
                                            bool packedRegisterAggregate) {
        if (!obj || !incomingValue) {
            error("direct value binding requires an incoming value");
        }
        auto bound = materializeBinding(obj);
        if (packedRegisterAggregate) {
            storeNativeAbiDirectValue(scope->builder, *typeMgr, obj->getType(),
                                      incomingValue, bound->getllvmValue());
        } else {
            scope->builder.CreateStore(incomingValue, bound->getllvmValue());
        }
        return bound;
    }

    std::vector<AstNode *> consumeArrayOuterDimension(
        const std::vector<AstNode *> &dims) {
        std::vector<AstNode *> remaining;
        remaining.reserve(dims.size());
        bool consumed = false;
        const bool legacyPrefix = isLegacyArrayDimensionPrefix(dims);
        for (auto *dim : dims) {
            if (dim == nullptr) {
                continue;
            }
            if (!consumed) {
                consumed = true;
                continue;
            }
            remaining.push_back(dim);
        }
        if (legacyPrefix && remaining.size() > 1) {
            remaining.insert(remaining.begin(), nullptr);
        }
        return remaining;
    }

    TypeClass *arrayInitChildType(ArrayType *arrayType) {
        if (!arrayType) {
            return nullptr;
        }
        bool ok = false;
        auto dims = arrayType->staticDimensions(&ok);
        if (!ok || dims.empty()) {
            return nullptr;
        }
        if (dims.size() == 1) {
            return arrayType->getElementType();
        }
        auto childDims = consumeArrayOuterDimension(arrayType->getDimensions());
        return typeMgr->createArrayType(arrayType->getElementType(),
                                        std::move(childDims));
    }

    void setLocation(const location &loc) {
        currentLocation = loc;
        hasCurrentLocation = true;
        applyDebugLocation(scope->builder, debug, debugSubprogram, loc);
    }

    void setLocation(HIRNode *node) {
        if (node) {
            setLocation(node->getLocation());
        }
    }

    void clearLocation() { clearDebugLocation(scope->builder); }

    const LoopContext &requireCurrentLoop() {
        if (loopStack.empty()) {
            error("loop control statement escaped semantic validation");
        }
        return loopStack.back();
    }

    ObjectPtr compileExpr(HIRExpr *expr) {
        if (!expr) {
            return nullptr;
        }
        if (auto *value = dynamic_cast<HIRValue *>(expr)) {
            return value->getValue();
        }
        if (auto *tuple = dynamic_cast<HIRTupleLiteral *>(expr)) {
            setLocation(tuple);
            auto *tupleType = asUnqualified<TupleType>(tuple->getType());
            if (!tupleType) {
                error("tuple literal is missing its tuple type");
            }
            auto *llvmTupleType = typeMgr->getLLVMType(tupleType);
            llvm::Value *aggregate = llvm::UndefValue::get(llvmTupleType);
            const auto &itemTypes = tupleType->getItemTypes();
            if (itemTypes.size() != tuple->getItems().size()) {
                error("tuple literal item count mismatch during lowering");
            }
            for (size_t i = 0; i < tuple->getItems().size(); ++i) {
                auto item = compileExpr(tuple->getItems()[i]);
                if (!item) {
                    error("tuple literal item did not produce a value");
                }
                auto *itemValue = item->get(scope);
                if (!isByteCopyCompatible(itemTypes[i], item->getType())) {
                    error("tuple literal item type mismatch during lowering");
                }
                aggregate = scope->builder.CreateInsertValue(
                    aggregate, itemValue, {static_cast<unsigned>(i)});
            }
            auto result =
                tupleType->newObj(Object::REG_VAL | Object::READONLY);
            result->bindllvmValue(aggregate);
            return result;
        }
        if (auto *structLiteral = dynamic_cast<HIRStructLiteral *>(expr)) {
            setLocation(structLiteral);
            auto *structType =
                asUnqualified<StructType>(structLiteral->getType());
            if (!structType) {
                error("struct literal is missing its struct type");
            }
            auto *llvmStructType = llvm::dyn_cast<llvm::StructType>(
                typeMgr->getLLVMType(structType));
            if (!llvmStructType) {
                error("struct literal lowering requires an LLVM struct type");
            }

            std::vector<std::pair<TypeClass *, int>> orderedMembers(
                structType->getMembers().size(), {nullptr, -1});
            for (const auto &member : structType->getMembers()) {
                const auto index = static_cast<size_t>(member.second.second);
                if (index >= orderedMembers.size()) {
                    error("struct literal member index is out of range");
                }
                orderedMembers[index] = member.second;
            }
            if (orderedMembers.size() != structLiteral->getFields().size()) {
                error("struct literal field count mismatch during lowering");
            }

            llvm::Value *aggregate = llvm::UndefValue::get(llvmStructType);
            for (size_t i = 0; i < structLiteral->getFields().size(); ++i) {
                auto field = compileExpr(structLiteral->getFields()[i]);
                if (!field) {
                    error("struct literal field did not produce a value");
                }
                auto *fieldType = orderedMembers[i].first;
                if (!isByteCopyCompatible(fieldType, field->getType())) {
                    error("struct literal field type mismatch during lowering");
                }
                aggregate = scope->builder.CreateInsertValue(
                    aggregate, field->get(scope), {static_cast<unsigned>(i)});
            }
            auto result =
                structType->newObj(Object::REG_VAL | Object::READONLY);
            result->bindllvmValue(aggregate);
            return result;
        }
        if (auto *arrayInit = dynamic_cast<HIRArrayInit *>(expr)) {
            setLocation(arrayInit);
            auto *arrayType = asUnqualified<ArrayType>(arrayInit->getType());
            if (!arrayType || !arrayType->hasStaticLayout()) {
                error("array initializer requires a fixed-layout array type");
            }
            auto *childType = arrayInitChildType(arrayType);
            if (!childType) {
                error("array initializer is missing its child element type");
            }
            llvm::Value *aggregate =
                llvm::Constant::getNullValue(scope->getLLVMType(arrayType));
            for (std::size_t i = 0; i < arrayInit->getItems().size(); ++i) {
                auto item = compileExpr(arrayInit->getItems()[i]);
                if (!item) {
                    error("array initializer item did not produce a value");
                }
                if (!isByteCopyCompatible(childType, item->getType())) {
                    error(
                        "array initializer item type mismatch during lowering");
                }
                aggregate = scope->builder.CreateInsertValue(
                    aggregate, item->get(scope), {static_cast<unsigned>(i)});
            }
            auto result =
                arrayType->newObj(Object::REG_VAL | Object::READONLY);
            result->bindllvmValue(aggregate);
            return result;
        }
        if (auto *byteString = dynamic_cast<HIRByteStringLiteral *>(expr)) {
            setLocation(byteString);
            auto *globalValue = getOrCreateByteStringGlobal(byteString);
            auto *llvmArrayType =
                llvm::cast<llvm::ArrayType>(globalValue->getValueType());
            auto *zero =
                llvm::ConstantInt::get(scope->builder.getInt32Ty(), 0, true);
            auto *borrowed = scope->builder.CreateInBoundsGEP(
                llvmArrayType, globalValue, {zero, zero});
            return makeReadonlyValue(byteString->getType(), borrowed);
        }
        if (auto *nullLiteral = dynamic_cast<HIRNullLiteral *>(expr)) {
            setLocation(nullLiteral);
            auto *type = nullLiteral->getType();
            if (!isPointerLikeType(type)) {
                error("null literal requires a concrete pointer type");
            }
            auto *value = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(scope->getLLVMType(type)));
            return makeReadonlyValue(type, value);
        }
        if (auto *cast = dynamic_cast<HIRNumericCast *>(expr)) {
            setLocation(cast);
            return emitNumericCast(cast);
        }
        if (auto *bitCast = dynamic_cast<HIRBitCast *>(expr)) {
            setLocation(bitCast);
            return emitBitCopyCast(bitCast);
        }
        if (auto *traitObjectCast = dynamic_cast<HIRTraitObjectCast *>(expr)) {
            setLocation(traitObjectCast);
            return emitTraitObjectCast(traitObjectCast);
        }
        if (auto *assign = dynamic_cast<HIRAssign *>(expr)) {
            setLocation(assign);
            auto dst = compileExpr(assign->getLeft());
            auto src = compileExpr(assign->getRight());
            if (!dst || !src) {
                error("assignment requires values");
            }
            dst->set(scope, src.get());
            return dst;
        }
        if (auto *bin = dynamic_cast<HIRBinOper *>(expr)) {
            setLocation(bin);
            if (bin->getBinding().shortCircuit) {
                return emitShortCircuitBinary(bin);
            }
            auto left = compileExpr(bin->getLeft());
            auto right = compileExpr(bin->getRight());
            return emitBinaryOperator(bin->getBinding(), left.get(),
                                      right.get());
        }
        if (auto *unary = dynamic_cast<HIRUnaryOper *>(expr)) {
            setLocation(unary);
            auto value = compileExpr(unary->getExpr());
            return emitUnaryOperator(unary->getBinding(), value.get());
        }
        if (auto *selector = dynamic_cast<HIRSelector *>(expr)) {
            setLocation(selector);
            auto parent = compileExpr(selector->getParent());
            auto fieldName = selector->getFieldName();
            if (auto *tupleParent = parent->as<TupleVar>()) {
                if (!selector->isValueFieldSelector()) {
                    error("tuple selectors do not support method calls");
                }
                return tupleParent->getField(scope, fieldName);
            }
            if (auto *structParent = parent->as<StructVar>()) {
                if (selector->isMethodSelector()) {
                    error(kMethodSelectorDirectCallError);
                }
                return structParent->getField(scope, fieldName);
            }
            error("selector parent must be a struct or tuple value");
        }
        if (auto *call = dynamic_cast<HIRCall *>(expr)) {
            setLocation(call);
            std::vector<ObjectPtr> args;
            llvm::Value *calleeValue = nullptr;
            FuncType *funcType = nullptr;
            bool hasImplicitSelf = false;

            if (auto *selector = dynamic_cast<HIRSelector *>(call->getCallee());
                selector && selector->isMethodSelector()) {
                auto parent = compileExpr(selector->getParent());
                Object *parentObj = parent.get();
                auto *structType = asUnqualified<StructType>(parentObj->getType());
                if (!structType) {
                    error("selector call parent must be a struct value");
                }
                const auto methodName = toStringRef(selector->getFieldName());
                auto *callee = scope->getMethodFunction(structType, methodName);
                if (callee) {
                    funcType = callee->getType()->as<FuncType>();
                    calleeValue = callee->getllvmValue();
                } else if (structType->isAppliedTemplateInstance() &&
                           structType->getMethodType(methodName)) {
                    auto *methodType = structType->getMethodType(methodName);
                    auto symbolName =
                        declarationsupport_impl::resolveStructMethodSymbolName(
                            structType, methodName);
                    auto *llvmFunc = scope->module.getFunction(symbolName);
                    if (methodType && llvmFunc) {
                        funcType = methodType;
                        calleeValue = llvmFunc;
                    }
                } else if (auto *traitMethodType =
                               structType->getTraitMethodTypeByKey(methodName)) {
                    auto *bound =
                        scope->getMethodFunction(structType, methodName);
                    if (bound) {
                        funcType = bound->getType()->as<FuncType>();
                        calleeValue = bound->getllvmValue();
                    } else {
                        funcType = traitMethodType;
                    }
                }
                if (!calleeValue || !funcType) {
                    error("unknown struct method");
                }
                ObjectPtr materializedParent;
                if (!parentObj->isVariable() || parentObj->isRegVal() ||
                    !parentObj->getllvmValue()) {
                    materializedParent =
                        materializeLocal(parentObj->getType(), parentObj);
                    parentObj = materializedParent.get();
                }
                auto *selfType = funcType && !funcType->getArgTypes().empty()
                                     ? funcType->getArgTypes().front()
                                     : nullptr;
                auto *selfPointeeType = getRawPointerPointeeType(selfType);
                if (!selfType || !selfPointeeType ||
                    !isConstQualificationConvertible(selfPointeeType,
                                                     parentObj->getType())) {
                    error("method lowering expected an implicit self pointer");
                }
                args.push_back(
                    makeReadonlyValue(selfType, parentObj->getllvmValue()));
                hasImplicitSelf = true;
            } else if (auto *callee =
                           getDirectFunctionCallee(call->getCallee())) {
                funcType = callee->getType()->as<FuncType>();
                calleeValue = callee->getllvmValue();
                hasImplicitSelf = callee->hasImplicitSelf();
            } else {
                auto calleeObj = compileExpr(call->getCallee());
                if (!calleeObj) {
                    error("call target did not produce a value");
                }
                funcType = getFunctionPointerTarget(calleeObj->getType());
                if (!funcType) {
                    error(
                        "callee must be a function, function pointer, or "
                        "method selector");
                }
                calleeValue = calleeObj->get(scope);
            }

            args.reserve(args.size() + call->getArgs().size());
            for (auto *arg : call->getArgs()) {
                auto value = compileExpr(arg);
                if (!value) {
                    error("call argument did not produce a value");
                }
                args.push_back(value);
            }
            return emitFunctionCall(scope, calleeValue, funcType, args,
                                    hasImplicitSelf);
        }
        if (auto *traitObjectCall = dynamic_cast<HIRTraitObjectCall *>(expr)) {
            setLocation(traitObjectCall);
            return emitTraitObjectCall(traitObjectCall);
        }
        if (auto *index = dynamic_cast<HIRIndex *>(expr)) {
            setLocation(index);
            auto target = compileExpr(index->getTarget());
            if (!target) {
                error("array indexing target did not produce a value");
            }
            auto *arrayType = asUnqualified<ArrayType>(target->getType());
            auto *indexableType =
                asUnqualified<IndexablePointerType>(target->getType());
            if (!arrayType && !indexableType) {
                error(
                    "array indexing expects an array value or indexable "
                    "pointer");
            }

            std::vector<llvm::Value *> gepIndices;
            llvm::Type *gepSourceType = nullptr;
            llvm::Value *targetPtr = nullptr;
            const bool fixedLayout = arrayType && arrayType->hasStaticLayout();
            if (arrayType && !fixedLayout) {
                error(
                    "array indexing requires a fixed-layout array type or an "
                    "indexable pointer");
            }
            gepIndices.reserve(index->getIndices().size() +
                               (fixedLayout ? 1 : 0));
            if (fixedLayout) {
                targetPtr = target->getllvmValue();
                if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
                    error("array indexing expects an addressable array value");
                }
                gepSourceType = scope->getLLVMType(arrayType);
                gepIndices.push_back(llvm::ConstantInt::get(
                    scope->builder.getInt32Ty(), 0, true));
            } else {
                targetPtr = target->get(scope);
                if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
                    error("array indexing expects a pointer value");
                }
                gepSourceType =
                    scope->getLLVMType(indexableType->getElementType());
            }
            for (auto *argExpr : index->getIndices()) {
                auto arg = compileExpr(argExpr);
                if (!arg || arg->getType() != i32Ty) {
                    error("array indexing expects `i32` indices");
                }
                gepIndices.push_back(arg->get(scope));
            }

            auto *resultType = index->getType();
            if (!resultType) {
                error("array indexing result type is missing");
            }
            auto *elementPtr = scope->builder.CreateInBoundsGEP(
                gepSourceType, targetPtr, gepIndices);
            auto result = resultType->newObj(Object::VARIABLE);
            result->setllvmValue(elementPtr);
            return result;
        }
        error("unsupported HIR expression");
    }

    ObjectPtr compileNode(HIRNode *node) {
        if (!node) {
            return nullptr;
        }
        if (auto *block = dynamic_cast<HIRBlock *>(node)) {
            return compileBlock(block);
        }
        if (auto *varDef = dynamic_cast<HIRVarDef *>(node)) {
            setLocation(varDef);
            ObjectPtr initVal;
            if (varDef->getInit()) {
                initVal = compileExpr(varDef->getInit());
            }
            auto obj = materializeBinding(varDef->getObject(), initVal.get());
            scope->addObj(varDef->getName(), obj);
            emitDebugDeclare(debug, funcScope, debugSubprogram, obj.get(),
                             toStringRef(varDef->getName()), obj->getType(),
                             varDef->getLocation());
            return obj;
        }
        if (auto *ret = dynamic_cast<HIRRet *>(node)) {
            setLocation(ret);
            auto *retSlot = funcScope->retVal();
            if (ret->getExpr()) {
                auto value = compileExpr(ret->getExpr());
                if (!retSlot) {
                    error(ret->getLocation(),
                          "unexpected return value in void function");
                }
                retSlot->set(scope, value.get());
            } else if (retSlot) {
                error(ret->getLocation(), "missing return value");
            }

            if (funcScope->retBlock()) {
                scope->builder.CreateBr(funcScope->retBlock());
            } else if (retSlot) {
                scope->builder.CreateRet(retSlot->get(scope));
            } else {
                scope->builder.CreateRetVoid();
            }
            funcScope->setReturned();
            return funcScope->retValObject();
        }
        if (auto *breakNode = dynamic_cast<HIRBreak *>(node)) {
            setLocation(breakNode);
            scope->builder.CreateBr(requireCurrentLoop().breakBlock);
            return nullptr;
        }
        if (auto *continueNode = dynamic_cast<HIRContinue *>(node)) {
            setLocation(continueNode);
            scope->builder.CreateBr(requireCurrentLoop().continueBlock);
            return nullptr;
        }
        if (auto *ifNode = dynamic_cast<HIRIf *>(node)) {
            setLocation(ifNode);
            auto condObj = compileExpr(ifNode->getCondition());
            auto *llvmFunc = scope->builder.GetInsertBlock()->getParent();

            auto *thenBB =
                llvm::BasicBlock::Create(context, "if.then", llvmFunc);
            auto *mergeBB = llvm::BasicBlock::Create(context, "if.end");
            auto *elseBB = ifNode->hasElseBlock()
                               ? llvm::BasicBlock::Create(context, "if.else")
                               : mergeBB;

            scope->builder.CreateCondBr(emitBoolCast(condObj.get()), thenBB,
                                        elseBB);

            scope->builder.SetInsertPoint(thenBB);
            compileBlock(ifNode->getThenBlock());
            if (!scope->builder.GetInsertBlock()->getTerminator()) {
                scope->builder.CreateBr(mergeBB);
            }

            if (ifNode->hasElseBlock()) {
                llvmFunc->insert(llvmFunc->end(), elseBB);
                scope->builder.SetInsertPoint(elseBB);
                compileBlock(ifNode->getElseBlock());
                if (!scope->builder.GetInsertBlock()->getTerminator()) {
                    scope->builder.CreateBr(mergeBB);
                }
            }

            llvmFunc->insert(llvmFunc->end(), mergeBB);
            scope->builder.SetInsertPoint(mergeBB);
            return nullptr;
        }
        if (auto *forNode = dynamic_cast<HIRFor *>(node)) {
            setLocation(forNode);
            auto *llvmFunc = scope->builder.GetInsertBlock()->getParent();
            auto *condBB =
                llvm::BasicBlock::Create(context, "for.cond", llvmFunc);
            auto *bodyBB = llvm::BasicBlock::Create(context, "for.body");
            auto *endBB = llvm::BasicBlock::Create(context, "for.end");
            auto *elseBB = forNode->hasElseBlock()
                               ? llvm::BasicBlock::Create(context, "for.else")
                               : endBB;

            scope->builder.CreateBr(condBB);

            scope->builder.SetInsertPoint(condBB);
            auto condObj = compileExpr(forNode->getCondition());
            scope->builder.CreateCondBr(emitBoolCast(condObj.get()), bodyBB,
                                        elseBB);

            llvmFunc->insert(llvmFunc->end(), bodyBB);
            scope->builder.SetInsertPoint(bodyBB);
            loopStack.push_back({condBB, endBB});
            compileBlock(forNode->getBody());
            loopStack.pop_back();
            if (!scope->builder.GetInsertBlock()->getTerminator()) {
                scope->builder.CreateBr(condBB);
            }

            if (forNode->hasElseBlock()) {
                llvmFunc->insert(llvmFunc->end(), elseBB);
                scope->builder.SetInsertPoint(elseBB);
                compileBlock(forNode->getElseBlock());
                if (!scope->builder.GetInsertBlock()->getTerminator()) {
                    scope->builder.CreateBr(endBB);
                }
            }

            llvmFunc->insert(llvmFunc->end(), endBB);
            scope->builder.SetInsertPoint(endBB);
            return nullptr;
        }
        if (auto *expr = dynamic_cast<HIRExpr *>(node)) {
            return compileExpr(expr);
        }
        error("unsupported HIR node");
    }

    ObjectPtr compileBlock(HIRBlock *block, bool introduceScope = true) {
        ObjectPtr last;
        if (!block) {
            return last;
        }

        auto *savedScope = scope;
        std::unique_ptr<LocalScope> nestedScope;
        if (introduceScope) {
            if (auto *parentLocal = dynamic_cast<LocalScope *>(scope)) {
                nestedScope = std::make_unique<LocalScope>(parentLocal);
            } else {
                nestedScope = std::make_unique<LocalScope>(funcScope);
            }
            scope = nestedScope.get();
        }
        for (auto *stmt : block->getBody()) {
            auto *insertBlock = scope->builder.GetInsertBlock();
            if (!insertBlock || insertBlock->getTerminator()) {
                break;
            }
            last = compileNode(stmt);
        }
        scope = savedScope;
        return last;
    }

    llvm::Value *emitBoolCast(Object *obj) {
        auto *value = obj->get(scope);
        auto *type = obj->getType();
        if (isBoolStorageType(type)) {
            return scope->builder.CreateICmpNE(
                value, llvm::ConstantInt::get(scope->getLLVMType(type), 0));
        }
        if (isIntegerType(type)) {
            return scope->builder.CreateICmpNE(
                value, llvm::ConstantInt::get(scope->getLLVMType(type), 0));
        }
        if (type == f32Ty) {
            return scope->builder.CreateFCmpUNE(
                value, llvm::ConstantFP::get(scope->getLLVMType(f32Ty), 0.0f));
        }
        if (type == f64Ty) {
            return scope->builder.CreateFCmpUNE(
                value, llvm::ConstantFP::get(scope->getLLVMType(f64Ty), 0.0));
        }
        if (isPointerLikeType(type)) {
            return scope->builder.CreateICmpNE(
                value, llvm::ConstantPointerNull::get(
                           llvm::cast<llvm::PointerType>(value->getType())));
        }
        error("unsupported condition type");
    }

    ObjectPtr makeReadonlyValue(TypeClass *type, llvm::Value *value) {
        if (isBoolStorageType(type) && value) {
            auto *boolLLVMType = scope->getLLVMType(boolTy);
            if (value->getType() != boolLLVMType) {
                if (value->getType()->isIntegerTy(1)) {
                    value = scope->builder.CreateZExt(value, boolLLVMType);
                } else if (value->getType()->isIntegerTy()) {
                    auto *isTrue = scope->builder.CreateICmpNE(
                        value, llvm::ConstantInt::get(value->getType(), 0));
                    value = scope->builder.CreateZExt(isTrue, boolLLVMType);
                } else {
                    error("bool value lowering expects an integer LLVM value");
                }
            }
        }
        auto obj = type->newObj(Object::REG_VAL | Object::READONLY);
        obj->bindllvmValue(value);
        return obj;
    }

    ObjectPtr emitNumericCast(HIRNumericCast *cast) {
        auto source = compileExpr(cast->getExpr());
        if (!source || !source->getType() || !cast->getType()) {
            error("numeric cast requires concrete source and target types");
        }

        auto *sourceType = source->getType();
        auto *targetType = cast->getType();
        auto *value = source->get(scope);
        if (sourceType == targetType) {
            return makeReadonlyValue(targetType, value);
        }

        llvm::Value *result = nullptr;
        if (isIntegerType(sourceType) && isIntegerType(targetType)) {
            auto sourceBits = static_cast<unsigned>(
                scope->types()->getTypeAllocSize(sourceType) * 8);
            auto targetBits = static_cast<unsigned>(
                scope->types()->getTypeAllocSize(targetType) * 8);
            if (sourceBits == targetBits) {
                result = value;
            } else if (sourceBits < targetBits) {
                result = isSignedIntegerType(sourceType)
                             ? scope->builder.CreateSExt(
                                   value, scope->getLLVMType(targetType))
                             : scope->builder.CreateZExt(
                                   value, scope->getLLVMType(targetType));
            } else {
                result = scope->builder.CreateTrunc(
                    value, scope->getLLVMType(targetType));
            }
        } else if (isFloatType(sourceType) && isFloatType(targetType)) {
            auto sourceBits = static_cast<unsigned>(
                scope->types()->getTypeAllocSize(sourceType) * 8);
            auto targetBits = static_cast<unsigned>(
                scope->types()->getTypeAllocSize(targetType) * 8);
            if (sourceBits == targetBits) {
                result = value;
            } else if (sourceBits < targetBits) {
                result = scope->builder.CreateFPExt(
                    value, scope->getLLVMType(targetType));
            } else {
                result = scope->builder.CreateFPTrunc(
                    value, scope->getLLVMType(targetType));
            }
        } else if (isIntegerType(sourceType) && isFloatType(targetType)) {
            result = isSignedIntegerType(sourceType)
                         ? scope->builder.CreateSIToFP(
                               value, scope->getLLVMType(targetType))
                         : scope->builder.CreateUIToFP(
                               value, scope->getLLVMType(targetType));
        } else if (isFloatType(sourceType) && isIntegerType(targetType)) {
            result = isSignedIntegerType(targetType)
                         ? scope->builder.CreateFPToSI(
                               value, scope->getLLVMType(targetType))
                         : scope->builder.CreateFPToUI(
                               value, scope->getLLVMType(targetType));
        } else {
            error("unsupported numeric cast");
        }

        return makeReadonlyValue(targetType, result);
    }

    ObjectPtr emitBitCopyCast(HIRBitCast *cast) {
        auto source = compileExpr(cast->getExpr());
        if (!source || !source->getType() || !cast->getType()) {
            error("bit-copy cast requires concrete source and target types");
        }

        auto *sourceType = source->getType();
        auto *targetType = cast->getType();
        auto *sourceValue = source->get(scope);
        if (sourceType == targetType) {
            return makeReadonlyValue(targetType, sourceValue);
        }

        auto bitWidthFor = [this](TypeClass *type) -> unsigned {
            auto byteSize = scope->types()->getTypeAllocSize(type);
            if (byteSize == 0) {
                error("bit-copy cast requires a concrete data layout size");
            }
            return static_cast<unsigned>(byteSize * 8);
        };

        auto targetBitsWidth = bitWidthFor(targetType);
        auto *targetBitsType = llvm::IntegerType::get(
            scope->builder.getContext(), targetBitsWidth);

        auto isBitsArray = [](TypeClass *type) {
            auto *array = asUnqualified<ArrayType>(type);
            return array && array->getElementType() == u8Ty &&
                   array->hasStaticLayout() &&
                   array->staticDimensions().size() == 1;
        };

        llvm::Value *bits = nullptr;
        unsigned bitsWidth = 0;
        if (sourceValue->getType()->isIntegerTy()) {
            bitsWidth = bitWidthFor(sourceType);
            auto *sourceBitsType =
                llvm::IntegerType::get(scope->builder.getContext(), bitsWidth);
            bits = sourceValue;
            if (bits->getType() != sourceBitsType) {
                bits =
                    scope->builder.CreateTruncOrBitCast(bits, sourceBitsType);
            }
        } else if (sourceValue->getType()->isFloatingPointTy()) {
            bitsWidth = bitWidthFor(sourceType);
            auto *sourceBitsType =
                llvm::IntegerType::get(scope->builder.getContext(), bitsWidth);
            bits = scope->builder.CreateBitCast(sourceValue, sourceBitsType);
        } else if (sourceValue->getType()->isPointerTy()) {
            bitsWidth = bitWidthFor(sourceType);
            auto *sourceBitsType =
                llvm::IntegerType::get(scope->builder.getContext(), bitsWidth);
            bits = scope->builder.CreatePtrToInt(sourceValue, sourceBitsType);
        } else if (isBitsArray(sourceType)) {
            auto dims =
                asUnqualified<ArrayType>(sourceType)->staticDimensions();
            const auto relevantBytes = std::max<std::int64_t>(
                1, std::min<std::int64_t>(
                       dims[0],
                       static_cast<std::int64_t>(
                           scope->types()->getTypeAllocSize(targetType))));
            bitsWidth = static_cast<unsigned>(relevantBytes * 8);
            auto *sourceBitsType =
                llvm::IntegerType::get(scope->builder.getContext(), bitsWidth);
            bits = llvm::ConstantInt::get(sourceBitsType, 0);
            for (std::int64_t i = 0; i < relevantBytes; ++i) {
                auto *byteValue = scope->builder.CreateExtractValue(
                    sourceValue, {static_cast<unsigned>(i)});
                auto *byteBits =
                    scope->builder.CreateZExt(byteValue, sourceBitsType);
                if (i != 0) {
                    byteBits = scope->builder.CreateShl(
                        byteBits,
                        llvm::ConstantInt::get(sourceBitsType, i * 8));
                }
                bits = scope->builder.CreateOr(bits, byteBits);
            }
        } else {
            error("unsupported source type for raw bit-copy");
        }

        if (bitsWidth < targetBitsWidth) {
            bits = scope->builder.CreateZExt(bits, targetBitsType);
        } else if (bitsWidth > targetBitsWidth) {
            bits = scope->builder.CreateTrunc(bits, targetBitsType);
        }

        auto *targetLLVMType = scope->getLLVMType(targetType);
        llvm::Value *result = nullptr;
        if (targetLLVMType->isIntegerTy()) {
            result = bits;
        } else if (targetLLVMType->isFloatingPointTy()) {
            result = scope->builder.CreateBitCast(bits, targetLLVMType);
        } else if (targetLLVMType->isPointerTy()) {
            result = scope->builder.CreateIntToPtr(bits, targetLLVMType);
        } else if (isBitsArray(targetType)) {
            result = llvm::UndefValue::get(targetLLVMType);
            auto dims =
                asUnqualified<ArrayType>(targetType)->staticDimensions();
            for (std::int64_t i = 0; i < dims[0]; ++i) {
                auto *shift = i == 0 ? bits
                                     : scope->builder.CreateLShr(
                                           bits, llvm::ConstantInt::get(
                                                     targetBitsType, i * 8));
                auto *byteValue =
                    scope->builder.CreateTrunc(shift, scope->getLLVMType(u8Ty));
                result = scope->builder.CreateInsertValue(
                    result, byteValue, {static_cast<unsigned>(i)});
            }
        } else {
            error("unsupported target type for raw bit-copy");
        }

        return makeReadonlyValue(targetType, result);
    }

    ObjectPtr emitUnaryOperator(const UnaryOperatorBinding &binding,
                                Object *value) {
        auto *llvmValue = binding.kind == UnaryOperatorKind::AddressOf
                              ? value->getllvmValue()
                              : (binding.kind == UnaryOperatorKind::Dereference
                                     ? value->get(scope)
                                     : value->get(scope));

        switch (binding.kind) {
            case UnaryOperatorKind::Identity:
                return value;
            case UnaryOperatorKind::Negate:
                return makeReadonlyValue(
                    binding.resultType,
                    binding.operandClass == OperatorOperandClass::Float
                        ? scope->builder.CreateFNeg(llvmValue)
                        : scope->builder.CreateNeg(llvmValue));
            case UnaryOperatorKind::LogicalNot:
                return makeReadonlyValue(
                    boolTy, scope->builder.CreateNot(emitBoolCast(value)));
            case UnaryOperatorKind::BitwiseNot:
                return makeReadonlyValue(binding.resultType,
                                         scope->builder.CreateNot(llvmValue));
            case UnaryOperatorKind::AddressOf:
                if (!value->isVariable() || value->isRegVal() || !llvmValue) {
                    error("address-of expects an addressable value");
                }
                return makeReadonlyValue(binding.resultType, llvmValue);
            case UnaryOperatorKind::Dereference: {
                auto result = binding.resultType->newObj(Object::VARIABLE);
                result->setllvmValue(llvmValue);
                return result;
            }
            default:
                error("unsupported unary operator binding");
        }
    }

    ObjectPtr emitBinaryOperator(const BinaryOperatorBinding &binding,
                                 Object *left, Object *right) {
        auto *lhs = left->get(scope);
        auto *rhs = right->get(scope);
        if (binding.leftClass == OperatorOperandClass::Bool &&
            binding.rightClass == OperatorOperandClass::Bool &&
            (binding.kind == BinaryOperatorKind::BitAnd ||
             binding.kind == BinaryOperatorKind::BitXor ||
             binding.kind == BinaryOperatorKind::BitOr)) {
            lhs = emitBoolCast(left);
            rhs = emitBoolCast(right);
        }
        llvm::Value *result = nullptr;

        switch (binding.kind) {
            case BinaryOperatorKind::Add:
                result = binding.leftClass == OperatorOperandClass::Float
                             ? scope->builder.CreateFAdd(lhs, rhs)
                             : scope->builder.CreateAdd(lhs, rhs);
                break;
            case BinaryOperatorKind::Sub:
                result = binding.leftClass == OperatorOperandClass::Float
                             ? scope->builder.CreateFSub(lhs, rhs)
                             : scope->builder.CreateSub(lhs, rhs);
                break;
            case BinaryOperatorKind::Mul:
                result = binding.leftClass == OperatorOperandClass::Float
                             ? scope->builder.CreateFMul(lhs, rhs)
                             : scope->builder.CreateMul(lhs, rhs);
                break;
            case BinaryOperatorKind::Div:
                if (binding.leftClass == OperatorOperandClass::Float) {
                    result = scope->builder.CreateFDiv(lhs, rhs);
                } else if (binding.leftClass ==
                           OperatorOperandClass::UnsignedInt) {
                    result = scope->builder.CreateUDiv(lhs, rhs);
                } else {
                    result = scope->builder.CreateSDiv(lhs, rhs);
                }
                break;
            case BinaryOperatorKind::Mod:
                result = binding.leftClass == OperatorOperandClass::UnsignedInt
                             ? scope->builder.CreateURem(lhs, rhs)
                             : scope->builder.CreateSRem(lhs, rhs);
                break;
            case BinaryOperatorKind::ShiftLeft:
                result = scope->builder.CreateShl(lhs, rhs);
                break;
            case BinaryOperatorKind::ShiftRight:
                result = binding.leftClass == OperatorOperandClass::UnsignedInt
                             ? scope->builder.CreateLShr(lhs, rhs)
                             : scope->builder.CreateAShr(lhs, rhs);
                break;
            case BinaryOperatorKind::BitAnd:
                result = scope->builder.CreateAnd(lhs, rhs);
                break;
            case BinaryOperatorKind::BitXor:
                result = scope->builder.CreateXor(lhs, rhs);
                break;
            case BinaryOperatorKind::BitOr:
                result = scope->builder.CreateOr(lhs, rhs);
                break;
            case BinaryOperatorKind::Less:
                if (binding.leftClass == OperatorOperandClass::Float) {
                    result = scope->builder.CreateFCmpOLT(lhs, rhs);
                } else if (binding.leftClass ==
                           OperatorOperandClass::UnsignedInt) {
                    result = scope->builder.CreateICmpULT(lhs, rhs);
                } else {
                    result = scope->builder.CreateICmpSLT(lhs, rhs);
                }
                break;
            case BinaryOperatorKind::Greater:
                if (binding.leftClass == OperatorOperandClass::Float) {
                    result = scope->builder.CreateFCmpOGT(lhs, rhs);
                } else if (binding.leftClass ==
                           OperatorOperandClass::UnsignedInt) {
                    result = scope->builder.CreateICmpUGT(lhs, rhs);
                } else {
                    result = scope->builder.CreateICmpSGT(lhs, rhs);
                }
                break;
            case BinaryOperatorKind::LessEqual:
                if (binding.leftClass == OperatorOperandClass::Float) {
                    result = scope->builder.CreateFCmpOLE(lhs, rhs);
                } else if (binding.leftClass ==
                           OperatorOperandClass::UnsignedInt) {
                    result = scope->builder.CreateICmpULE(lhs, rhs);
                } else {
                    result = scope->builder.CreateICmpSLE(lhs, rhs);
                }
                break;
            case BinaryOperatorKind::GreaterEqual:
                if (binding.leftClass == OperatorOperandClass::Float) {
                    result = scope->builder.CreateFCmpOGE(lhs, rhs);
                } else if (binding.leftClass ==
                           OperatorOperandClass::UnsignedInt) {
                    result = scope->builder.CreateICmpUGE(lhs, rhs);
                } else {
                    result = scope->builder.CreateICmpSGE(lhs, rhs);
                }
                break;
            case BinaryOperatorKind::Equal:
                result = binding.leftClass == OperatorOperandClass::Float
                             ? scope->builder.CreateFCmpOEQ(lhs, rhs)
                             : scope->builder.CreateICmpEQ(lhs, rhs);
                break;
            case BinaryOperatorKind::NotEqual:
                result = binding.leftClass == OperatorOperandClass::Float
                             ? scope->builder.CreateFCmpUNE(lhs, rhs)
                             : scope->builder.CreateICmpNE(lhs, rhs);
                break;
            default:
                error("unsupported binary operator binding");
        }

        return makeReadonlyValue(binding.resultType, result);
    }

    ObjectPtr emitShortCircuitBinary(HIRBinOper *bin) {
        const auto &binding = bin->getBinding();
        auto left = compileExpr(bin->getLeft());
        auto *lhsBool = emitBoolCast(left.get());

        auto &context = scope->builder.getContext();
        auto *function = scope->builder.GetInsertBlock()
                             ? scope->builder.GetInsertBlock()->getParent()
                             : nullptr;
        if (!function) {
            error("logical short-circuit needs an active function");
        }

        auto *lhsBlock = scope->builder.GetInsertBlock();
        auto *rhsBB = llvm::BasicBlock::Create(context, "logic.rhs", function);
        auto *shortBB =
            llvm::BasicBlock::Create(context, "logic.short", function);
        auto *mergeBB =
            llvm::BasicBlock::Create(context, "logic.merge", function);

        if (binding.kind == BinaryOperatorKind::LogicalAnd) {
            scope->builder.CreateCondBr(lhsBool, rhsBB, shortBB);
        } else {
            scope->builder.CreateCondBr(lhsBool, shortBB, rhsBB);
        }

        scope->builder.SetInsertPoint(shortBB);
        scope->builder.CreateBr(mergeBB);
        auto *shortEnd = scope->builder.GetInsertBlock();

        scope->builder.SetInsertPoint(rhsBB);
        auto right = compileExpr(bin->getRight());
        auto *rhsBool = emitBoolCast(right.get());
        scope->builder.CreateBr(mergeBB);
        auto *rhsEnd = scope->builder.GetInsertBlock();

        scope->builder.SetInsertPoint(mergeBB);
        auto *phi = scope->builder.CreatePHI(scope->builder.getInt1Ty(), 2);
        phi->addIncoming(
            llvm::ConstantInt::getFalse(scope->builder.getInt1Ty()),
            binding.kind == BinaryOperatorKind::LogicalAnd ? shortEnd : rhsEnd);
        phi->addIncoming(
            llvm::ConstantInt::getTrue(scope->builder.getInt1Ty()),
            binding.kind == BinaryOperatorKind::LogicalOr ? shortEnd : rhsEnd);
        if (binding.kind == BinaryOperatorKind::LogicalAnd) {
            phi->setIncomingValue(1, rhsBool);
        } else {
            phi->setIncomingValue(0, rhsBool);
        }
        (void)lhsBlock;
        return makeReadonlyValue(boolTy, phi);
    }

    void ensureTerminatorForCurrentBlock() {
        auto *block = scope->builder.GetInsertBlock();
        if (!block || block->getTerminator()) {
            return;
        }
        if (funcScope->retBlock() && block != funcScope->retBlock()) {
            scope->builder.CreateBr(funcScope->retBlock());
            return;
        }
        if (funcScope->retVal()) {
            if (returnByPointer) {
                scope->builder.CreateRetVoid();
            } else if (abiSignature.resultInfo.packedRegisterAggregate) {
                auto *retSlot = funcScope->retVal();
                llvm::Value *retValue = nullptr;
                if (retSlot->isVariable() && !retSlot->isRegVal() &&
                    retSlot->getllvmValue()) {
                    retValue = loadNativeAbiDirectValue(
                        scope->builder, *typeMgr, retSlot->getType(),
                        retSlot->getllvmValue());
                } else {
                    retValue = packNativeAbiDirectValue(
                        scope->builder, *typeMgr, retSlot->getType(),
                        retSlot->get(scope));
                }
                scope->builder.CreateRet(retValue);
            } else {
                scope->builder.CreateRet(funcScope->retVal()->get(scope));
            }
        } else {
            scope->builder.CreateRetVoid();
        }
    }

    void finalizeModuleEntryResult() {
        if (!moduleInitState || !moduleInitResult || !scope ||
            !funcScope->retVal()) {
            return;
        }
        auto *resultValue = funcScope->retVal()->get(scope);
        scope->builder.CreateStore(resultValue, moduleInitResult);
        scope->builder.CreateStore(scope->builder.getInt32(2), moduleInitState);
    }

    void emitModuleInitFailure(llvm::Value *resultValue) {
        assert(moduleInitState);
        assert(moduleInitResult);
        scope->builder.CreateStore(resultValue, moduleInitResult);
        scope->builder.CreateStore(scope->builder.getInt32(2), moduleInitState);
        scope->builder.CreateRet(resultValue);
    }

    void emitModuleEntryPrologue(HIRFunc *hirFunc, llvm::Function *llvmFunc) {
        if (!hirFunc->isTopLevelEntry() || !unit) {
            return;
        }
        if (hirFunc->getFuncType()->getRetType() != i32Ty ||
            !hirFunc->getParams().empty() || hirFunc->hasSelfBinding()) {
            functionError(hirFunc,
                          "module entry must use the canonical `() -> i32` "
                          "signature");
        }

        moduleInitState = getOrCreateModuleInitState(global, *unit);
        moduleInitResult = getOrCreateModuleInitResult(global, *unit);

        auto *entryBB = scope->builder.GetInsertBlock();
        auto *doneBB =
            llvm::BasicBlock::Create(context, "module.init.done", llvmFunc);
        auto *checkRunningBB = llvm::BasicBlock::Create(
            context, "module.init.check_running", llvmFunc);
        auto *runningBB =
            llvm::BasicBlock::Create(context, "module.init.reentry", llvmFunc);
        auto *runBB =
            llvm::BasicBlock::Create(context, "module.init.run", llvmFunc);

        auto *stateValue = scope->builder.CreateLoad(
            scope->builder.getInt32Ty(), moduleInitState, "module.init.state");
        auto *isDone = scope->builder.CreateICmpEQ(
            stateValue, scope->builder.getInt32(2), "module.init.done_flag");
        scope->builder.CreateCondBr(isDone, doneBB, checkRunningBB);

        scope->builder.SetInsertPoint(doneBB);
        auto *cachedResult = scope->builder.CreateLoad(
            scope->builder.getInt32Ty(), moduleInitResult,
            "module.init.cached_result");
        scope->builder.CreateRet(cachedResult);

        scope->builder.SetInsertPoint(checkRunningBB);
        auto *runningState = scope->builder.CreateLoad(
            scope->builder.getInt32Ty(), moduleInitState,
            "module.init.running_state");
        auto *isRunning = scope->builder.CreateICmpEQ(
            runningState, scope->builder.getInt32(1),
            "module.init.running_flag");
        scope->builder.CreateCondBr(isRunning, runningBB, runBB);

        scope->builder.SetInsertPoint(runningBB);
        scope->builder.CreateRet(scope->builder.getInt32(1));

        scope->builder.SetInsertPoint(runBB);
        scope->builder.CreateStore(scope->builder.getInt32(1), moduleInitState);

        if (!moduleGraph) {
            return;
        }

        for (const auto &dependencyPath :
             moduleGraph->dependenciesOf(unit->path())) {
            auto *dependencyUnit = moduleGraph->find(dependencyPath);
            if (!dependencyUnit) {
                functionError(
                    hirFunc,
                    "module dependency is missing from the module graph");
            }

            auto *depInit = getOrCreateModuleInitDeclaration(global, typeMgr,
                                                             *dependencyUnit);
            auto *depResult =
                scope->builder.CreateCall(depInit, {}, "module.init.dep");
            auto *depOkBB = llvm::BasicBlock::Create(
                context, "module.init.dep_ok", llvmFunc);
            auto *depFailBB = llvm::BasicBlock::Create(
                context, "module.init.dep_fail", llvmFunc);
            auto *depSucceeded = scope->builder.CreateICmpEQ(
                depResult, scope->builder.getInt32(0),
                "module.init.dep_success");
            scope->builder.CreateCondBr(depSucceeded, depOkBB, depFailBB);

            scope->builder.SetInsertPoint(depFailBB);
            emitModuleInitFailure(depResult);

            scope->builder.SetInsertPoint(depOkBB);
        }
    }

public:
    FunctionCompiler(TypeTable *typeMgr, GlobalScope *global, HIRFunc *hirFunc,
                     ByteStringGlobalCache &byteStringGlobals,
                     DebugInfoContext *debug = nullptr,
                     const CompilationUnit *unit = nullptr,
                     const ModuleGraph *moduleGraph = nullptr)
        : typeMgr(typeMgr),
          global(global),
          scope(nullptr),
          funcScope(nullptr),
          context(global->module.getContext()),
          debug(debug),
          unit(unit),
          moduleGraph(moduleGraph),
          byteStringGlobals_(byteStringGlobals) {
        if (!hirFunc) {
            error("missing HIR function");
        }

        auto *llvmFunc = hirFunc->getLLVMFunction();
        if (!llvmFunc) {
            functionError(hirFunc, "HIR function missing LLVM function");
        }
        if (!llvmFunc->empty()) {
            return;
        }
        if (!hirFunc->getBody()) {
            return;
        }

        auto *entry = llvm::BasicBlock::Create(context, "entry", llvmFunc);
        global->builder.SetInsertPoint(entry);
        funcScope = new FuncScope(global);
        scope = funcScope;

        auto *funcType = hirFunc->getFuncType();
        if (!funcType) {
            functionError(hirFunc, "invalid function type");
        }
        abiSignature =
            classifyFunctionAbi(*typeMgr, funcType, hirFunc->hasSelfBinding());
        returnByPointer = abiSignature.hasIndirectResult;

        if (hirFunc->hasSelfBinding()) {
            funcScope->structTy =
                asUnqualified<StructType>(getRawPointerPointeeType(
                    hirFunc->getSelfBinding().object->getType()));
        }

        if (debug) {
            debugSubprogram = createDebugSubprogram(*debug, llvmFunc, funcType,
                                                    llvmFunc->getName().str(),
                                                    hirFunc->getLocation());
            scope->builder.SetCurrentDebugLocation(llvm::DILocation::get(
                context, sourceLine(hirFunc->getLocation()),
                sourceColumn(hirFunc->getLocation()), debugSubprogram));
        }

        auto *retType = funcType->getRetType();
        if (!hirFunc->isTopLevelEntry() && retType &&
            !hirFunc->hasGuaranteedReturn()) {
            functionError(hirFunc, "not all paths return a value");
        }

        size_t llvmArgIndex = 0;
        if (hirFunc->hasSelfBinding()) {
            auto &binding = hirFunc->getSelfBinding();
            auto argIt = llvmFunc->arg_begin();
            std::advance(argIt, llvmArgIndex);
            ObjectPtr selfObj;
            const auto &argInfo = abiSignature.argInfo(0);
            auto passKind = argInfo.passKind;
            if (passKind == AbiPassKind::IndirectRef) {
                auto incomingSelf =
                    binding.object->getType()->newObj(Object::VARIABLE);
                incomingSelf->setllvmValue(&*argIt);
                selfObj = materializeBinding(binding.object, incomingSelf.get());
            } else if (passKind == AbiPassKind::IndirectValue) {
                selfObj =
                    materializeIndirectValueBinding(binding.object, &*argIt);
            } else {
                selfObj = materializeDirectValueBinding(
                    binding.object, &*argIt, argInfo.packedRegisterAggregate);
            }
            scope->addObj(binding.name, selfObj);
            emitDebugDeclare(debug, funcScope, debugSubprogram, selfObj.get(),
                             toStringRef(binding.name), selfObj->getType(),
                             binding.loc, 1);
            ++llvmArgIndex;
        }

        if (retType) {
            ObjectPtr retSlot;
            if (returnByPointer) {
                if (llvmFunc->arg_size() < 1) {
                    functionError(
                        hirFunc,
                        "function is missing hidden return slot argument");
                }
                auto argIt = llvmFunc->arg_begin();
                std::advance(argIt, llvmArgIndex);
                retSlot = retType->newObj(Object::VARIABLE);
                retSlot->setllvmValue(&*argIt);
                ++llvmArgIndex;
            } else {
                retSlot = materializeLocal(retType, nullptr);
            }
            funcScope->initRetVal(retSlot);
            if (hirFunc->isTopLevelEntry() && retType == i32Ty) {
                auto defaultResult = ObjectPtr(new ConstVar(i32Ty, int32_t(0)));
                retSlot->set(scope, defaultResult.get());
            }
            auto *retBB = llvm::BasicBlock::Create(context, "return", llvmFunc);
            funcScope->initRetBlock(retBB);
        }

        emitModuleEntryPrologue(hirFunc, llvmFunc);

        unsigned debugArgIndex = hirFunc->hasSelfBinding() ? 2 : 1;

        auto expectedArgs =
            abiSignature.llvmType ? abiSignature.llvmType->getNumParams() : 0;
        if (llvmFunc->arg_size() != expectedArgs) {
            functionError(hirFunc, "function argument number mismatch");
        }
        const std::size_t sourceParamOffset = hirFunc->hasSelfBinding() ? 1 : 0;
        std::size_t sourceParamIndex = sourceParamOffset;
        for (const auto &binding : hirFunc->getParams()) {
            auto argIt = llvmFunc->arg_begin();
            std::advance(argIt, llvmArgIndex);
            ObjectPtr argObj;
            const auto &argInfo = abiSignature.argInfo(sourceParamIndex);
            auto passKind = argInfo.passKind;
            if (passKind == AbiPassKind::IndirectRef) {
                auto incomingArg =
                    binding.object->getType()->newObj(Object::VARIABLE);
                incomingArg->setllvmValue(&*argIt);
                argObj = materializeBinding(binding.object, incomingArg.get());
            } else if (passKind == AbiPassKind::IndirectValue) {
                argObj =
                    materializeIndirectValueBinding(binding.object, &*argIt);
            } else {
                argObj = materializeDirectValueBinding(
                    binding.object, &*argIt, argInfo.packedRegisterAggregate);
            }
            scope->addObj(binding.name, argObj);
            emitDebugDeclare(debug, funcScope, debugSubprogram, argObj.get(),
                             toStringRef(binding.name), argObj->getType(),
                             binding.loc, debugArgIndex);
            ++llvmArgIndex;
            ++debugArgIndex;
            ++sourceParamIndex;
        }

        compileBlock(hirFunc->getBody(), false);

        if (funcScope->retBlock()) {
            auto *insertBlock = scope->builder.GetInsertBlock();
            if (insertBlock && !insertBlock->getTerminator()) {
                scope->builder.CreateBr(funcScope->retBlock());
            }
            scope->builder.SetInsertPoint(funcScope->retBlock());
            if (debugSubprogram) {
                scope->builder.SetCurrentDebugLocation(llvm::DILocation::get(
                    context, sourceLine(hirFunc->getLocation()),
                    sourceColumn(hirFunc->getLocation()), debugSubprogram));
            }
            finalizeModuleEntryResult();
        }

        ensureTerminatorForCurrentBlock();
        clearLocation();
    }
};

class ModuleCompiler {
    GlobalScope *global;
    TypeTable *typeMgr;
    DebugInfoContext *debug;
    const CompilationUnit *unit;
    const ModuleGraph *moduleGraph;
    ByteStringGlobalCache byteStringGlobals_;

public:
    ModuleCompiler(GlobalScope *global, HIRModule *module,
                   DebugInfoContext *debug = nullptr,
                   const CompilationUnit *unit = nullptr,
                   const ModuleGraph *moduleGraph = nullptr)
        : global(global),
          typeMgr(declarationsupport_impl::requireTypeTable(global)),
          debug(debug),
          unit(unit),
          moduleGraph(moduleGraph) {
        for (auto *func : module->getFunctions()) {
            FunctionCompiler(typeMgr, global, func, byteStringGlobals_, debug,
                             unit, moduleGraph);
        }
    }
};

}  // namespace llvmcodegen_impl

void
compileModule(Scope *global, AstNode *root, bool emitDebugInfo) {
    auto *globalScope = dynamic_cast<GlobalScope *>(global);
    assert(globalScope);
    initBuildinType(globalScope);

    auto resolvedModule = resolveModule(globalScope, root, nullptr, true);
    auto hirModule = analyzeModule(globalScope, *resolvedModule, nullptr);
    emitHIRModule(global, hirModule.get(), emitDebugInfo,
                  globalScope->module.getName().str(), nullptr, nullptr);
}

void
emitHIRModule(Scope *global, HIRModule *module, bool emitDebugInfo,
              const std::string &primarySourcePath, const CompilationUnit *unit,
              const ModuleGraph *moduleGraph) {
    auto *globalScope = dynamic_cast<GlobalScope *>(global);
    assert(globalScope);
    initBuildinType(globalScope);

    std::unique_ptr<llvmcodegen_impl::DebugInfoContext> debug;
    if (emitDebugInfo) {
        debug = std::make_unique<llvmcodegen_impl::DebugInfoContext>(
            globalScope->module, *globalScope->types(),
            primarySourcePath.empty() ? globalScope->module.getName().str()
                                      : primarySourcePath);
    }
    llvmcodegen_impl::ModuleCompiler(globalScope, module, debug.get(), unit,
                                     moduleGraph);

    if (debug) {
        debug->finalize();
    }
}

}  // namespace lona
