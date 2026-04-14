#include "lona/ast/astnode.hh"
#include "lona/declare/support.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/sema/hir.hh"
#include "lona/sema/initializer.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include <array>
#include <cassert>
#include <cstdint>
#include <llvm-18/llvm/IR/BasicBlock.h>
#include <llvm-18/llvm/IR/ConstantFold.h>
#include <llvm-18/llvm/IR/Constants.h>
#include <llvm-18/llvm/IR/DerivedTypes.h>
#include <llvm-18/llvm/IR/Module.h>
#include <llvm-18/llvm/IR/Type.h>
#include <string>
#include <utility>
#include <vector>

namespace lona {
namespace globaldefinition_impl {

AstStatList *
requireTopLevelBody(CompilationUnit &unit) {
    auto *tree = unit.requireSyntaxTree();
    if (auto *program = dynamic_cast<AstProgram *>(tree)) {
        return program->body;
    }
    if (auto *body = dynamic_cast<AstStatList *>(tree)) {
        return body;
    }
    internalError("compilation unit `" + toStdString(unit.path()) +
                      "` does not have a top-level statement list",
                  "This looks like a compiler parser/session integration bug.");
}

class GlobalDefinitionEmitter {
    GlobalScope *global_;
    TypeTable *typeMgr_;
    llvm::LLVMContext &context_;
    HIRModule ownerModule_;

    [[noreturn]] void error(const location &loc, const std::string &message,
                            const std::string &hint = std::string()) {
        lona::error(loc, message, hint);
    }

    std::string nextByteStringGlobalName() {
        static std::uint64_t nextId = 0;
        return ".lona.global.bytes." + std::to_string(nextId++);
    }

    llvm::Constant *buildByteStringArrayConstant(const ::string &bytes) {
        std::vector<std::uint8_t> data;
        data.reserve(bytes.size() + 1);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            data.push_back(static_cast<std::uint8_t>(
                static_cast<unsigned char>(bytes[i])));
        }
        data.push_back(0);
        return llvm::ConstantDataArray::get(context_, data);
    }

    llvm::Constant *createByteStringPointerConstant(const ::string &bytes) {
        auto *initializer = buildByteStringArrayConstant(bytes);
        auto *llvmArrayType =
            llvm::cast<llvm::ArrayType>(initializer->getType());
        auto *globalValue =
            new llvm::GlobalVariable(global_->module, llvmArrayType, true,
                                     llvm::GlobalValue::PrivateLinkage,
                                     initializer, nextByteStringGlobalName());
        globalValue->setAlignment(llvm::MaybeAlign(1));
        globalValue->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        auto *zero =
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0, true);
        std::array<llvm::Constant *, 2> indices = {zero, zero};
        return llvm::ConstantExpr::getInBoundsGetElementPtr(
            llvmArrayType, globalValue, indices);
    }

    llvm::Constant *emitScalarValue(HIRValue *value, const location &loc,
                                    const std::string &name) {
        auto *object = value ? value->getValue().get() : nullptr;
        auto *constant =
            object
                ? llvm::dyn_cast_or_null<llvm::Constant>(object->get(global_))
                : nullptr;
        if (!constant) {
            error(loc,
                  "global `" + name +
                      "` initializer escaped static-literal lowering",
                  "This looks like a compiler global-initializer bug.");
        }
        return constant;
    }

    llvm::Constant *foldCastConstant(unsigned opcode, llvm::Constant *source,
                                     llvm::Type *targetLLVMType,
                                     const location &loc,
                                     const std::string &name,
                                     llvm::StringRef context) {
        if (!source || !targetLLVMType) {
            error(loc,
                  "global `" + name + "` initializer escaped " + context.str() +
                      " lowering",
                  "This looks like a compiler global-initializer bug.");
        }
        if (source->getType() == targetLLVMType) {
            return source;
        }

        if (auto *folded = llvm::ConstantFoldCastInstruction(opcode, source,
                                                             targetLLVMType)) {
            return folded;
        }

        error(loc,
              "global `" + name + "` initializer escaped " + context.str() +
                  " lowering",
              "This looks like a compiler global-initializer bug.");
    }

    llvm::Constant *emitNumericCast(HIRNumericCast *cast, const location &loc,
                                    const std::string &name) {
        auto *sourceType =
            cast && cast->getExpr() ? cast->getExpr()->getType() : nullptr;
        auto *targetType = cast ? cast->getType() : nullptr;
        auto *source = cast && cast->getExpr()
                           ? emitAnalyzed(cast->getExpr(), loc, name)
                           : nullptr;
        if (!sourceType || !targetType || !source) {
            error(loc,
                  "global `" + name +
                      "` initializer escaped numeric cast lowering",
                  "This looks like a compiler global-initializer bug.");
        }

        auto *targetLLVMType = typeMgr_->getLLVMType(targetType);
        if (source->getType() == targetLLVMType) {
            return source;
        }
        if (isIntegerType(sourceType) && isIntegerType(targetType)) {
            auto sourceBits = static_cast<unsigned>(
                typeMgr_->getTypeAllocSize(sourceType) * 8);
            auto targetBits = static_cast<unsigned>(
                typeMgr_->getTypeAllocSize(targetType) * 8);
            if (sourceBits == targetBits) {
                return source;
            }
            if (sourceBits > targetBits) {
                return foldCastConstant(llvm::Instruction::Trunc, source,
                                        targetLLVMType, loc, name, "numeric");
            }
            return foldCastConstant(
                isSignedIntegerType(sourceType) ? llvm::Instruction::SExt
                                                : llvm::Instruction::ZExt,
                source, targetLLVMType, loc, name, "numeric");
        }
        if (isFloatType(sourceType) && isFloatType(targetType)) {
            auto sourceBits = static_cast<unsigned>(
                typeMgr_->getTypeAllocSize(sourceType) * 8);
            auto targetBits = static_cast<unsigned>(
                typeMgr_->getTypeAllocSize(targetType) * 8);
            if (sourceBits == targetBits) {
                return source;
            }
            if (sourceBits < targetBits) {
                return foldCastConstant(llvm::Instruction::FPExt, source,
                                        targetLLVMType, loc, name, "numeric");
            }
            return foldCastConstant(llvm::Instruction::FPTrunc, source,
                                    targetLLVMType, loc, name, "numeric");
        }
        if (isIntegerType(sourceType) && isFloatType(targetType)) {
            return foldCastConstant(
                isSignedIntegerType(sourceType) ? llvm::Instruction::SIToFP
                                                : llvm::Instruction::UIToFP,
                source, targetLLVMType, loc, name, "numeric");
        }
        if (isFloatType(sourceType) && isIntegerType(targetType)) {
            return foldCastConstant(
                isSignedIntegerType(targetType) ? llvm::Instruction::FPToSI
                                                : llvm::Instruction::FPToUI,
                source, targetLLVMType, loc, name, "numeric");
        }
        error(loc,
              "global `" + name + "` initializer escaped numeric cast lowering",
              "This looks like a compiler global-initializer bug.");
    }

    llvm::Constant *emitPointerCast(HIRBitCast *cast, const location &loc,
                                    const std::string &name) {
        auto *source = cast && cast->getExpr()
                           ? emitAnalyzed(cast->getExpr(), loc, name)
                           : nullptr;
        auto *targetType = cast ? cast->getType() : nullptr;
        if (!source || !targetType) {
            error(loc,
                  "global `" + name +
                      "` initializer escaped pointer cast lowering",
                  "This looks like a compiler global-initializer bug.");
        }

        auto *targetLLVMType = typeMgr_->getLLVMType(targetType);
        if (source->getType() == targetLLVMType) {
            return source;
        }
        if (source->getType()->isPointerTy() && targetLLVMType->isPointerTy()) {
            return llvm::ConstantExpr::getBitCast(source, targetLLVMType);
        }
        error(loc,
              "global `" + name + "` initializer escaped pointer cast lowering",
              "This looks like a compiler global-initializer bug.");
    }

    llvm::Constant *emitUnary(HIRUnaryOper *unary, const location &loc,
                              const std::string &name) {
        auto *operand = unary && unary->getExpr()
                            ? emitAnalyzed(unary->getExpr(), loc, name)
                            : nullptr;
        if (!operand) {
            error(loc,
                  "global `" + name + "` initializer escaped unary lowering",
                  "This looks like a compiler global-initializer bug.");
        }

        switch (unary->getBinding().kind) {
            case UnaryOperatorKind::Identity:
                return operand;
            case UnaryOperatorKind::Negate:
                if (operand->getType()->isFloatingPointTy()) {
                    return llvm::ConstantExpr::get(
                        llvm::Instruction::FSub,
                        llvm::ConstantFP::get(operand->getType(), -0.0),
                        operand);
                }
                if (operand->getType()->isIntegerTy()) {
                    return llvm::ConstantExpr::getNeg(operand);
                }
                break;
            default:
                break;
        }
        error(loc, "global `" + name + "` initializer escaped unary lowering",
              "This looks like a compiler global-initializer bug.");
    }

    HIRExpr *analyze(TypeClass *expectedType, AstNode *init,
                     const location &loc, const std::string &name) {
        if (!expectedType || !init) {
            return nullptr;
        }
        if (dynamic_cast<AstBraceInit *>(init)) {
            error(loc, "global `" + name + "` initializer is not supported yet",
                  "This first version only supports literal global "
                  "initializers. Add runtime initialization inside a function "
                  "for aggregate values.");
        }
        if (!isSupportedStaticLiteralInitializerExpr(init)) {
            error(
                loc,
                "global `" + name +
                    "` initializer must be a static literal expression",
                "This first version supports numbers, booleans, chars, "
                "strings, `null`, and unary `+` / `-` over numeric literals.");
        }

        auto *expr = analyzeStaticLiteralInitializerExpr(
            typeMgr_, &ownerModule_, init, expectedType);
        expr = coerceNumericInitializerExpr(typeMgr_, &ownerModule_, expr,
                                            expectedType, loc, false);
        expr = coercePointerInitializerExpr(typeMgr_, &ownerModule_, expr,
                                            expectedType, loc);
        requireCompatibleInitializerTypes(
            loc, expectedType, expr ? expr->getType() : nullptr,
            "global `" + name + "` initializer type mismatch");
        return expr;
    }

    llvm::Constant *emitAnalyzed(HIRExpr *expr, const location &loc,
                                 const std::string &name) {
        if (auto *value = dynamic_cast<HIRValue *>(expr)) {
            return emitScalarValue(value, loc, name);
        }
        if (auto *byteString = dynamic_cast<HIRByteStringLiteral *>(expr)) {
            return createByteStringPointerConstant(byteString->getBytes());
        }
        if (auto *nullLiteral = dynamic_cast<HIRNullLiteral *>(expr)) {
            auto *type = nullLiteral->getType();
            if (!isPointerLikeType(type)) {
                error(loc,
                      "global `" + name +
                          "` null initializer lost its pointer type",
                      "This looks like a compiler global-initializer bug.");
            }
            return llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(typeMgr_->getLLVMType(type)));
        }
        if (auto *numericCast = dynamic_cast<HIRNumericCast *>(expr)) {
            return emitNumericCast(numericCast, loc, name);
        }
        if (auto *bitCast = dynamic_cast<HIRBitCast *>(expr)) {
            return emitPointerCast(bitCast, loc, name);
        }
        if (auto *unary = dynamic_cast<HIRUnaryOper *>(expr)) {
            return emitUnary(unary, loc, name);
        }
        error(
            loc,
            "global `" + name + "` initializer escaped static-literal lowering",
            "This looks like a compiler global-initializer bug.");
    }

public:
    explicit GlobalDefinitionEmitter(GlobalScope *global)
        : global_(global),
          typeMgr_(declarationsupport_impl::requireTypeTable(global)),
          context_(global->module.getContext()) {}

    llvm::Constant *emit(TypeClass *expectedType, AstNode *init,
                         const location &loc, const std::string &name) {
        return emitAnalyzed(analyze(expectedType, init, loc, name), loc, name);
    }
};

}  // namespace globaldefinition_impl

void
defineUnitGlobals(Scope *global, CompilationUnit &unit) {
    auto *globalScope = dynamic_cast<GlobalScope *>(global);
    assert(globalScope);
    initBuildinType(globalScope);
    moduleinterface_impl::ensureUnitInterfaceCollected(unit);

    globaldefinition_impl::GlobalDefinitionEmitter emitter(globalScope);
    auto *body = globaldefinition_impl::requireTopLevelBody(unit);
    for (auto *stmt : body->getBody()) {
        auto *globalDecl = dynamic_cast<AstGlobalDecl *>(stmt);
        if (!globalDecl || globalDecl->isExtern()) {
            continue;
        }

        auto *runtimeName =
            unit.findLocalGlobal(toStdString(globalDecl->getName()));
        if (!runtimeName) {
            internalError(
                "failed to look up materialized global `" +
                    toStdString(globalDecl->getName()) +
                    "` in compilation unit `" + toStdString(unit.path()) + "`",
                "Collect declarations before defining global storage.");
        }

        auto *obj = globalScope->getObj(*runtimeName);
        if (!obj || !obj->getType() || !obj->getllvmValue()) {
            internalError(
                "failed to materialize global storage for `" +
                    toStdString(*runtimeName) + "`",
                "Collect declarations before defining global storage.");
        }

        auto *llvmGlobal =
            llvm::dyn_cast<llvm::GlobalVariable>(obj->getllvmValue());
        if (!llvmGlobal) {
            internalError(
                "global `" + toStdString(*runtimeName) +
                    "` does not map to an LLVM global variable",
                "Collect declarations before defining global storage.");
        }

        auto *initializer =
            emitter.emit(obj->getType(), globalDecl->getInitVal(),
                         globalDecl->loc, toStdString(globalDecl->getName()));
        llvmGlobal->setInitializer(initializer);
        llvmGlobal->setConstant(!isFullyWritableValueType(obj->getType()));
    }
}

}  // namespace lona
