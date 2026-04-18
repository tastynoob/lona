#include "lona/sema/initializer.hh"

#include "lona/err/err.hh"
#include "lona/module/module_interface.hh"
#include "lona/sema/injectedmember.hh"
#include "lona/type/buildin.hh"
#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace lona {
namespace initializer_semantics_impl {

[[noreturn]] void
raiseError(const location &loc, const std::string &message,
           const std::string &hint = std::string()) {
    lona::error(loc, message, hint);
}

std::string
describeResolvedFuncType(FuncType *type, size_t argOffset = 0) {
    if (type == nullptr) {
        return "<unknown type>";
    }

    std::string name = "(";
    const auto &argTypes = type->getArgTypes();
    for (size_t i = argOffset; i < argTypes.size(); ++i) {
        if (i != argOffset) {
            name += ", ";
        }
        if (type->getArgBindingKind(i) == BindingKind::Ref) {
            name += "ref ";
        }
        name += describeResolvedType(argTypes[i]);
    }
    name += ":";
    if (type->getRetType() != nullptr) {
        name += " ";
        name += describeResolvedType(type->getRetType());
    }
    name += ")";
    return name;
}

template<typename T, typename... Args>
T *
makeHIR(HIRModule *ownerModule, Args &&...args) {
    assert(ownerModule);
    return ownerModule->create<T>(std::forward<Args>(args)...);
}

bool
canIndexablePointerConvertTo(TypeClass *targetType, TypeClass *sourceType) {
    auto *targetElement = getIndexablePointerElementType(targetType);
    auto *sourceElement = getIndexablePointerElementType(sourceType);
    if (targetElement && sourceElement) {
        return isConstQualificationConvertible(targetElement, sourceElement);
    }
    if (isRawMemoryPointerType(targetType) &&
        isRawMemoryPointerType(sourceType)) {
        return isConstQualificationConvertible(
            getRawPointerPointeeType(targetType),
            getRawPointerPointeeType(sourceType));
    }
    if (targetElement && isRawMemoryPointerType(sourceType)) {
        return isConstQualificationConvertible(
            targetElement, getRawPointerPointeeType(sourceType));
    }
    if (sourceElement && isRawMemoryPointerType(targetType)) {
        return isConstQualificationConvertible(
            getRawPointerPointeeType(targetType), sourceElement);
    }
    return false;
}

TypeClass *
getPointerStorageElementType(TypeClass *type) {
    if (auto *element = getIndexablePointerElementType(type)) {
        return element;
    }
    return getRawPointerPointeeType(type);
}

bool
preservesConstQualificationForPointerRebind(TypeClass *targetType,
                                            TypeClass *sourceType) {
    if (!targetType || !sourceType) {
        return false;
    }

    auto *targetConst = targetType->as<ConstType>();
    auto *sourceConst = sourceType->as<ConstType>();
    if (sourceConst) {
        return targetConst &&
               preservesConstQualificationForPointerRebind(
                   targetConst->getBaseType(), sourceConst->getBaseType());
    }
    if (targetConst) {
        return preservesConstQualificationForPointerRebind(
            targetConst->getBaseType(), sourceType);
    }

    auto *targetNested = getPointerStorageElementType(targetType);
    auto *sourceNested = getPointerStorageElementType(sourceType);
    if (targetNested && sourceNested) {
        return preservesConstQualificationForPointerRebind(targetNested,
                                                           sourceNested);
    }
    return true;
}

TypeClass *
getConstU8Type(TypeTable *typeMgr) {
    return typeMgr ? typeMgr->createConstType(u8Ty) : nullptr;
}

string
getByteStringBytes(AstConst *node) {
    auto *bytes = node ? node->getBuf<string>() : nullptr;
    if (!bytes) {
        return {};
    }
    return *bytes;
}

std::uint8_t
getAsciiCharByte(AstConst *node) {
    auto bytes = getByteStringBytes(node);
    if (bytes.size() != 1) {
        raiseError(node ? node->loc : location(),
                   "character literal must contain exactly one ASCII byte",
                   "Use `'A'`, `'\\n'`, or a string literal like \"...\" for "
                   "UTF-8 or multi-byte text.");
    }
    const auto byte = static_cast<unsigned char>(bytes[0]);
    if (byte > 0x7F) {
        raiseError(node ? node->loc : location(),
                   "character literal must be ASCII",
                   "Only single-byte ASCII characters are allowed here. Use a "
                   "string literal like \"...\" for UTF-8 text.");
    }
    return static_cast<std::uint8_t>(byte);
}

HIRExpr *
analyzeConst(TypeTable *typeMgr, HIRModule *ownerModule, AstConst *node,
             TypeClass *expectedType = nullptr) {
    auto errorUnaryMinusOnlySignedMin = [&](const char *typeName) -> void {
        raiseError(
            node->loc,
            "integer literal magnitude is only valid with unary `-` for `" +
                std::string(typeName) + "`",
            "Write `-" + std::to_string(node->getDeferredSignedMinMagnitude()) +
                "_" + typeName + "` if you want the minimum `" +
                std::string(typeName) + "` value.");
    };

    switch (node->getType()) {
        case AstConst::Type::I8:
            if (node->isUnaryMinusOnlySignedMinLiteral()) {
                errorUnaryMinusOnlySignedMin("i8");
            }
            return makeHIR<HIRValue>(
                ownerModule, new ConstVar(i8Ty, *node->getBuf<std::int8_t>()),
                node->loc);
        case AstConst::Type::U8:
            return makeHIR<HIRValue>(
                ownerModule, new ConstVar(u8Ty, *node->getBuf<std::uint8_t>()),
                node->loc);
        case AstConst::Type::I16:
            if (node->isUnaryMinusOnlySignedMinLiteral()) {
                errorUnaryMinusOnlySignedMin("i16");
            }
            return makeHIR<HIRValue>(
                ownerModule, new ConstVar(i16Ty, *node->getBuf<std::int16_t>()),
                node->loc);
        case AstConst::Type::U16:
            return makeHIR<HIRValue>(
                ownerModule,
                new ConstVar(u16Ty, *node->getBuf<std::uint16_t>()), node->loc);
        case AstConst::Type::I32:
            if (node->isUnaryMinusOnlySignedMinLiteral()) {
                errorUnaryMinusOnlySignedMin("i32");
            }
            return makeHIR<HIRValue>(
                ownerModule, new ConstVar(i32Ty, *node->getBuf<std::int32_t>()),
                node->loc);
        case AstConst::Type::U32:
            return makeHIR<HIRValue>(
                ownerModule,
                new ConstVar(u32Ty, *node->getBuf<std::uint32_t>()), node->loc);
        case AstConst::Type::I64:
            if (node->isUnaryMinusOnlySignedMinLiteral()) {
                errorUnaryMinusOnlySignedMin("i64");
            }
            return makeHIR<HIRValue>(
                ownerModule, new ConstVar(i64Ty, *node->getBuf<std::int64_t>()),
                node->loc);
        case AstConst::Type::U64:
            return makeHIR<HIRValue>(
                ownerModule,
                new ConstVar(u64Ty, *node->getBuf<std::uint64_t>()), node->loc);
        case AstConst::Type::USIZE: {
            const auto value = *node->getBuf<std::uint64_t>();
            const auto usizeBytes = typeMgr->getTypeAllocSize(usizeTy);
            const auto usizeBits = static_cast<unsigned>(usizeBytes * 8);
            if (usizeBits < 64 &&
                value > ((std::uint64_t{1} << usizeBits) - 1)) {
                raiseError(node->loc,
                           "integer literal is out of range for `usize` on the "
                           "active target",
                           "Use a smaller `usize` literal, or switch to a "
                           "wider target pointer size.");
            }
            return makeHIR<HIRValue>(ownerModule, new ConstVar(usizeTy, value),
                                     node->loc);
        }
        case AstConst::Type::BOOL:
            return makeHIR<HIRValue>(
                ownerModule, new ConstVar(boolTy, *node->getBuf<bool>()),
                node->loc);
        case AstConst::Type::F32:
            return makeHIR<HIRValue>(
                ownerModule, new ConstVar(f32Ty, *node->getBuf<float>()),
                node->loc);
        case AstConst::Type::F64: {
            auto value = *node->getBuf<double>();
            auto *contextualType =
                expectedType ? stripTopLevelConst(expectedType) : nullptr;
            auto *contextualBase = asUnqualified<BaseType>(contextualType);
            const bool expectsF32 =
                contextualBase && contextualBase->type == BaseType::F32;
            const bool expectsF64 =
                contextualBase && contextualBase->type == BaseType::F64;
            if (!node->hasExplicitNumericType() && expectsF32) {
                return makeHIR<HIRValue>(
                    ownerModule, new ConstVar(f32Ty, static_cast<float>(value)),
                    node->loc);
            }
            if (expectsF32) {
                return makeHIR<HIRValue>(ownerModule,
                                         new ConstVar(f64Ty, value), node->loc);
            }
            if (contextualType == nullptr || expectsF64) {
                return makeHIR<HIRValue>(ownerModule,
                                         new ConstVar(f64Ty, value), node->loc);
            }
            raiseError(
                node->loc,
                "floating-point literal doesn't match the expected target type",
                "Use a `f32` or `f64` destination. For numeric conversion, "
                "call `cast[T](expr)` like `cast[i32](1.5)`. For raw bits, "
                "call `.tobits()` and keep the resulting `u8[N]` array.");
        }
        case AstConst::Type::STRING: {
            auto bytes = getByteStringBytes(node);
            auto *type = typeMgr ? typeMgr->createIndexablePointerType(
                                       getConstU8Type(typeMgr))
                                 : nullptr;
            return makeHIR<HIRByteStringLiteral>(ownerModule, std::move(bytes),
                                                 type, node->loc);
        }
        case AstConst::Type::CHAR:
            return makeHIR<HIRValue>(ownerModule,
                                     new ConstVar(u8Ty, getAsciiCharByte(node)),
                                     node->loc);
        case AstConst::Type::NULLPTR:
            if (expectedType && !isPointerLikeType(expectedType)) {
                raiseError(node->loc,
                           "`null` can only be used with pointer types",
                           nullLiteralHint());
            }
            return makeHIR<HIRNullLiteral>(ownerModule, expectedType,
                                           node->loc);
        default:
            raiseError(node->loc, "unsupported constant literal");
    }
}

HIRExpr *
analyzeUnary(TypeTable *typeMgr, HIRModule *ownerModule, AstUnaryOper *node,
             TypeClass *expectedType) {
    if (node->op == '-') {
        auto *constant = node->expr ? node->expr->as<AstConst>() : nullptr;
        if (constant && constant->isUnaryMinusOnlySignedMinLiteral()) {
            switch (constant->getType()) {
                case AstConst::Type::I8:
                    return makeHIR<HIRValue>(
                        ownerModule,
                        new ConstVar(i8Ty,
                                     std::numeric_limits<std::int8_t>::min()),
                        node->loc);
                case AstConst::Type::I16:
                    return makeHIR<HIRValue>(
                        ownerModule,
                        new ConstVar(i16Ty,
                                     std::numeric_limits<std::int16_t>::min()),
                        node->loc);
                case AstConst::Type::I32:
                    return makeHIR<HIRValue>(
                        ownerModule,
                        new ConstVar(i32Ty,
                                     std::numeric_limits<std::int32_t>::min()),
                        node->loc);
                case AstConst::Type::I64:
                    return makeHIR<HIRValue>(
                        ownerModule,
                        new ConstVar(i64Ty,
                                     std::numeric_limits<std::int64_t>::min()),
                        node->loc);
                default:
                    break;
            }
        }
    }

    TypeClass *contextualOperandType =
        expectedType && isNumericType(expectedType) ? expectedType : nullptr;
    auto *value = analyzeStaticLiteralInitializerExpr(
        typeMgr, ownerModule, node->expr, contextualOperandType);
    OperatorResolver operatorResolver(typeMgr);
    auto binding = operatorResolver.resolveUnary(node->op, value->getType(),
                                                 false, node->loc);
    return makeHIR<HIRUnaryOper>(ownerModule, binding, value,
                                 binding.resultType, node->loc);
}

}  // namespace initializer_semantics_impl

std::string
describeResolvedType(TypeClass *type) {
    using namespace initializer_semantics_impl;
    if (type == nullptr) {
        return "<unknown type>";
    }
    if (auto *qualified = type->as<ConstType>()) {
        return describeResolvedType(qualified->getBaseType()) + " const";
    }
    if (auto *pointer = type->as<PointerType>()) {
        if (auto *func = pointer->getPointeeType()->as<FuncType>()) {
            return describeResolvedFuncType(func);
        }
        return describeResolvedType(pointer->getPointeeType()) + "*";
    }
    if (auto *indexable = type->as<IndexablePointerType>()) {
        return describeResolvedType(indexable->getElementType()) + "[*]";
    }
    if (auto *array = type->as<ArrayType>()) {
        return toStdString(array->full_name);
    }
    if (auto *func = type->as<FuncType>()) {
        return describeResolvedFuncType(func);
    }
    return toStdString(type->full_name);
}

std::string
numericConversionHint() {
    return "Integer-to-integer and float-to-float convert implicitly. "
           "Integer/float cross-conversion requires an explicit "
           "`cast[T](expr)` such as `cast[f32](1)` or `cast[i32](1.5)`.";
}

std::string
bitCopyHint() {
    return "Use `.tobits()` when you want raw bit-copy behavior. It returns "
           "`u8[N]`, and `u8[N].toXXX()` converts those bytes back with "
           "truncation or zero-fill.";
}

std::string
nullLiteralHint() {
    return "Use `null` only with pointer values such as `T*` or `T[*]`, for "
           "example `var p i32* = null` or `if p == null`.";
}

std::string
pointerConversionHint() {
    return "Only conversions between `T[*]` and matching raw pointers `T*` are "
           "implicit. Integer-to-pointer and pointer-to-integer require an "
           "explicit `cast[T](expr)`.";
}

bool
isNullLiteralExpr(HIRExpr *expr) {
    return dynamic_cast<HIRNullLiteral *>(expr) != nullptr;
}

bool
isRawMemoryPointerType(TypeClass *type) {
    auto *pointee = getRawPointerPointeeType(type);
    return pointee != nullptr && pointee->as<FuncType>() == nullptr;
}

bool
isIndexablePointerType(TypeClass *type) {
    return getIndexablePointerElementType(type) != nullptr;
}

bool
canImplicitPointerViewConversion(TypeClass *targetType, TypeClass *sourceType) {
    return initializer_semantics_impl::canIndexablePointerConvertTo(targetType,
                                                                    sourceType);
}

bool
canExplicitPointerRebindCast(TypeClass *targetType, TypeClass *sourceType) {
    if (!(isRawMemoryPointerType(targetType) ||
          isIndexablePointerType(targetType)) ||
        !(isRawMemoryPointerType(sourceType) ||
          isIndexablePointerType(sourceType))) {
        return false;
    }

    auto *targetElement =
        initializer_semantics_impl::getPointerStorageElementType(targetType);
    auto *sourceElement =
        initializer_semantics_impl::getPointerStorageElementType(sourceType);
    return targetElement && sourceElement &&
           initializer_semantics_impl::
               preservesConstQualificationForPointerRebind(targetElement,
                                                           sourceElement);
}

bool
canExplicitPointerIntegerCast(TypeClass *targetType, TypeClass *sourceType) {
    if (!targetType || !sourceType || targetType == sourceType) {
        return false;
    }

    const bool targetPointer = isRawMemoryPointerType(targetType) ||
                               isIndexablePointerType(targetType);
    const bool sourcePointer = isRawMemoryPointerType(sourceType) ||
                               isIndexablePointerType(sourceType);
    return (targetPointer && isIntegerType(sourceType)) ||
           (sourcePointer && isIntegerType(targetType));
}

bool
isSupportedStaticLiteralInitializerExpr(AstNode *node) {
    if (node == nullptr) {
        return false;
    }
    if (dynamic_cast<AstConst *>(node)) {
        return true;
    }
    if (auto *unary = dynamic_cast<AstUnaryOper *>(node)) {
        return (unary->op == '+' || unary->op == '-') &&
               isSupportedStaticLiteralInitializerExpr(unary->expr);
    }
    return false;
}

TypeClass *
inferStaticLiteralInitializerType(ModuleInterface *interface, AstNode *init) {
    if (!interface || !init) {
        return nullptr;
    }
    if (auto *constant = dynamic_cast<AstConst *>(init)) {
        switch (constant->getType()) {
            case AstConst::Type::I8:
                return i8Ty;
            case AstConst::Type::U8:
            case AstConst::Type::CHAR:
                return u8Ty;
            case AstConst::Type::I16:
                return i16Ty;
            case AstConst::Type::U16:
                return u16Ty;
            case AstConst::Type::I32:
                return i32Ty;
            case AstConst::Type::U32:
                return u32Ty;
            case AstConst::Type::I64:
                return i64Ty;
            case AstConst::Type::U64:
                return u64Ty;
            case AstConst::Type::USIZE:
                return usizeTy;
            case AstConst::Type::F32:
                return f32Ty;
            case AstConst::Type::F64:
                return f64Ty;
            case AstConst::Type::BOOL:
                return boolTy;
            case AstConst::Type::STRING:
                return interface->getOrCreateIndexablePointerType(
                    interface->getOrCreateConstType(u8Ty));
            case AstConst::Type::NULLPTR:
                return nullptr;
        }
    }
    if (auto *unary = dynamic_cast<AstUnaryOper *>(init)) {
        if (unary->op == '+' || unary->op == '-') {
            return inferStaticLiteralInitializerType(interface, unary->expr);
        }
    }
    return nullptr;
}

void
requireCompatibleInitializerTypes(const location &loc, TypeClass *expectedType,
                                  TypeClass *actualType,
                                  const std::string &context) {
    if (auto *expectedDyn = asUnqualified<DynTraitType>(expectedType)) {
        if (auto *actualReadOnlyDyn = getReadOnlyDynTraitType(actualType)) {
            if (!expectedDyn->hasReadOnlyDataPtr() &&
                actualReadOnlyDyn->traitName() == expectedDyn->traitName()) {
                initializer_semantics_impl::raiseError(
                    loc,
                    context + ": writable `" +
                        describeResolvedType(expectedDyn) +
                        "` cannot receive a read-only trait object",
                    "This trait object was borrowed from a const receiver. "
                    "Borrow a writable value before constructing `" +
                        describeResolvedType(expectedDyn) +
                        "`, or make the destination accept a read-only trait "
                        "object instead.");
            }
        }
    }
    if (expectedType && actualType &&
        isByteCopyCompatible(expectedType, actualType)) {
        return;
    }
    initializer_semantics_impl::raiseError(
        loc,
        context + ": expected " + describeResolvedType(expectedType) +
            ", got " + describeResolvedType(actualType),
        numericConversionHint() + " " + bitCopyHint() + " " +
            pointerConversionHint());
}

HIRExpr *
coerceNumericInitializerExpr(TypeTable *typeMgr, HIRModule *ownerModule,
                             HIRExpr *expr, TypeClass *targetType,
                             const location &loc, bool explicitRequest) {
    if (!expr || !targetType) {
        return expr;
    }
    auto *sourceType = expr->getType();
    if (!sourceType || sourceType == targetType) {
        return expr;
    }
    if (isConstQualificationConvertible(
            targetType, materializeValueType(typeMgr, sourceType))) {
        return expr;
    }
    if (explicitRequest) {
        if (canExplicitNumericConversion(targetType, sourceType)) {
            return initializer_semantics_impl::makeHIR<HIRNumericCast>(
                ownerModule, expr, targetType, true, loc);
        }
        initializer_semantics_impl::raiseError(
            loc,
            "explicit numeric conversion is not available from `" +
                describeResolvedType(sourceType) + "` to `" +
                describeResolvedType(targetType) + "`",
            numericConversionHint());
    }
    if (canImplicitNumericConversion(targetType, sourceType)) {
        return initializer_semantics_impl::makeHIR<HIRNumericCast>(
            ownerModule, expr, targetType, false, loc);
    }
    return expr;
}

HIRExpr *
coercePointerInitializerExpr(TypeTable *typeMgr, HIRModule *ownerModule,
                             HIRExpr *expr, TypeClass *targetType,
                             const location &loc, bool explicitCast) {
    if (!expr || !targetType) {
        return expr;
    }
    if (isNullLiteralExpr(expr)) {
        if (isPointerLikeType(targetType)) {
            if (expr->getType() == targetType) {
                return expr;
            }
            return initializer_semantics_impl::makeHIR<HIRNullLiteral>(
                ownerModule, targetType, loc);
        }
        return expr;
    }
    auto *sourceType = expr->getType();
    if (!sourceType || sourceType == targetType) {
        return expr;
    }
    if ((isRawMemoryPointerType(targetType) ||
         isIndexablePointerType(targetType)) &&
        (isRawMemoryPointerType(sourceType) ||
         isIndexablePointerType(sourceType)) &&
        isConstQualificationConvertible(
            targetType, materializeValueType(typeMgr, sourceType))) {
        return initializer_semantics_impl::makeHIR<HIRBitCast>(
            ownerModule, expr, targetType, loc);
    }
    if (explicitCast && canExplicitPointerRebindCast(targetType, sourceType)) {
        return initializer_semantics_impl::makeHIR<HIRBitCast>(
            ownerModule, expr, targetType, loc);
    }
    if (canImplicitPointerViewConversion(targetType, sourceType)) {
        return initializer_semantics_impl::makeHIR<HIRBitCast>(
            ownerModule, expr, targetType, loc);
    }
    return expr;
}

HIRExpr *
analyzeStaticLiteralInitializerExpr(TypeTable *typeMgr, HIRModule *ownerModule,
                                    AstNode *node, TypeClass *expectedType) {
    if (auto *constant = dynamic_cast<AstConst *>(node)) {
        return initializer_semantics_impl::analyzeConst(typeMgr, ownerModule,
                                                        constant, expectedType);
    }
    if (auto *unary = dynamic_cast<AstUnaryOper *>(node)) {
        return initializer_semantics_impl::analyzeUnary(typeMgr, ownerModule,
                                                        unary, expectedType);
    }
    initializer_semantics_impl::raiseError(
        node ? node->loc : location(),
        "static initializer expression must be a literal or unary `+` / `-` "
        "over a literal");
}

}  // namespace lona
