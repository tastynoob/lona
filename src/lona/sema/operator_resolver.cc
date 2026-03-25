#include "operator_resolver.hh"

#include "lona/err/err.hh"
#include "lona/type/buildin.hh"
#include "parser.hh"
#include <array>

namespace lona {
namespace {

[[noreturn]] void
error(const location &loc, const std::string &message,
      const std::string &hint = std::string()) {
    throw DiagnosticError(DiagnosticError::Category::Semantic, loc, message, hint);
}

std::string
toStdString(const string &value) {
    return {value.tochara(), value.size()};
}

std::string
describeType(TypeClass *type) {
    if (!type) {
        return "<unknown type>";
    }
    return toStdString(type->full_name);
}

struct UnaryOperatorRule {
    token_type token;
    OperatorOperandClass operandClass;
    UnaryOperatorKind kind;
    bool resultIsBool = false;
};

struct BinaryOperatorRule {
    token_type token;
    OperatorOperandClass leftClass;
    OperatorOperandClass rightClass;
    BinaryOperatorKind kind;
    bool requireSameType = true;
    bool resultIsBool = false;
    bool shortCircuit = false;
};

constexpr std::array<UnaryOperatorRule, 6> kUnaryRules = {{
    {'+', OperatorOperandClass::SignedInt, UnaryOperatorKind::Identity, false},
    {'+', OperatorOperandClass::UnsignedInt, UnaryOperatorKind::Identity, false},
    {'+', OperatorOperandClass::Float, UnaryOperatorKind::Identity, false},
    {'-', OperatorOperandClass::SignedInt, UnaryOperatorKind::Negate, false},
    {'-', OperatorOperandClass::UnsignedInt, UnaryOperatorKind::Negate, false},
    {'-', OperatorOperandClass::Float, UnaryOperatorKind::Negate, false},
}};

constexpr std::array<BinaryOperatorRule, 33> kBinaryRules = {{
    {'+', OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::Add},
    {'+', OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::Add},
    {'+', OperatorOperandClass::Float, OperatorOperandClass::Float, BinaryOperatorKind::Add},
    {'-', OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::Sub},
    {'-', OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::Sub},
    {'-', OperatorOperandClass::Float, OperatorOperandClass::Float, BinaryOperatorKind::Sub},
    {'*', OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::Mul},
    {'*', OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::Mul},
    {'*', OperatorOperandClass::Float, OperatorOperandClass::Float, BinaryOperatorKind::Mul},
    {'/', OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::Div},
    {'/', OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::Div},
    {'/', OperatorOperandClass::Float, OperatorOperandClass::Float, BinaryOperatorKind::Div},
    {'%', OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::Mod},
    {'%', OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::Mod},
    {Parser::token::SHIFT_LEFT, OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::ShiftLeft},
    {Parser::token::SHIFT_LEFT, OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::ShiftLeft},
    {Parser::token::SHIFT_RIGHT, OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::ShiftRight},
    {Parser::token::SHIFT_RIGHT, OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::ShiftRight},
    {'&', OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::BitAnd},
    {'&', OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::BitAnd},
    {'&', OperatorOperandClass::Bool, OperatorOperandClass::Bool, BinaryOperatorKind::BitAnd},
    {'^', OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::BitXor},
    {'^', OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::BitXor},
    {'^', OperatorOperandClass::Bool, OperatorOperandClass::Bool, BinaryOperatorKind::BitXor},
    {'|', OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::BitOr},
    {'|', OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::BitOr},
    {'|', OperatorOperandClass::Bool, OperatorOperandClass::Bool, BinaryOperatorKind::BitOr},
    {'<', OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::Less, true, true},
    {'<', OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::Less, true, true},
    {'<', OperatorOperandClass::Float, OperatorOperandClass::Float, BinaryOperatorKind::Less, true, true},
    {'>', OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::Greater, true, true},
    {'>', OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::Greater, true, true},
    {'>', OperatorOperandClass::Float, OperatorOperandClass::Float, BinaryOperatorKind::Greater, true, true},
}};

constexpr std::array<BinaryOperatorRule, 13> kExtendedBinaryRules = {{
    {Parser::token::LOGIC_LE, OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::LessEqual, true, true},
    {Parser::token::LOGIC_LE, OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::LessEqual, true, true},
    {Parser::token::LOGIC_LE, OperatorOperandClass::Float, OperatorOperandClass::Float, BinaryOperatorKind::LessEqual, true, true},
    {Parser::token::LOGIC_GE, OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::GreaterEqual, true, true},
    {Parser::token::LOGIC_GE, OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::GreaterEqual, true, true},
    {Parser::token::LOGIC_GE, OperatorOperandClass::Float, OperatorOperandClass::Float, BinaryOperatorKind::GreaterEqual, true, true},
    {Parser::token::LOGIC_EQUAL, OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::Equal, true, true},
    {Parser::token::LOGIC_EQUAL, OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::Equal, true, true},
    {Parser::token::LOGIC_EQUAL, OperatorOperandClass::Float, OperatorOperandClass::Float, BinaryOperatorKind::Equal, true, true},
    {Parser::token::LOGIC_EQUAL, OperatorOperandClass::Bool, OperatorOperandClass::Bool, BinaryOperatorKind::Equal, true, true},
    {Parser::token::LOGIC_EQUAL, OperatorOperandClass::Pointer, OperatorOperandClass::Pointer, BinaryOperatorKind::Equal, true, true},
    {Parser::token::LOGIC_NOT_EQUAL, OperatorOperandClass::Bool, OperatorOperandClass::Bool, BinaryOperatorKind::NotEqual, true, true},
    {Parser::token::LOGIC_NOT_EQUAL, OperatorOperandClass::Pointer, OperatorOperandClass::Pointer, BinaryOperatorKind::NotEqual, true, true},
}};

constexpr std::array<BinaryOperatorRule, 4> kNotEqualNumericRules = {{
    {Parser::token::LOGIC_NOT_EQUAL, OperatorOperandClass::SignedInt, OperatorOperandClass::SignedInt, BinaryOperatorKind::NotEqual, true, true},
    {Parser::token::LOGIC_NOT_EQUAL, OperatorOperandClass::UnsignedInt, OperatorOperandClass::UnsignedInt, BinaryOperatorKind::NotEqual, true, true},
    {Parser::token::LOGIC_NOT_EQUAL, OperatorOperandClass::Float, OperatorOperandClass::Float, BinaryOperatorKind::NotEqual, true, true},
    {Parser::token::LOGIC_AND, OperatorOperandClass::TruthyScalar, OperatorOperandClass::TruthyScalar, BinaryOperatorKind::LogicalAnd, false, true, true},
}};

constexpr std::array<BinaryOperatorRule, 1> kLogicalOrRule = {{
    {Parser::token::LOGIC_OR, OperatorOperandClass::TruthyScalar, OperatorOperandClass::TruthyScalar, BinaryOperatorKind::LogicalOr, false, true, true},
}};

bool
matchesOperandClass(TypeClass *type, OperatorOperandClass operandClass) {
    switch (operandClass) {
    case OperatorOperandClass::SignedInt:
        return isSignedIntegerType(type);
    case OperatorOperandClass::UnsignedInt:
        return isUnsignedIntegerType(type);
    case OperatorOperandClass::Float:
        return isFloatType(type);
    case OperatorOperandClass::Bool:
        return isBoolStorageType(type);
    case OperatorOperandClass::Pointer:
        return isPointerLikeType(type);
    case OperatorOperandClass::TruthyScalar:
        return isTruthyScalarType(type);
    default:
        return false;
    }
}

const UnaryOperatorRule *
findUnaryRule(token_type token, TypeClass *operandType) {
    for (const auto &rule : kUnaryRules) {
        if (rule.token != token) {
            continue;
        }
        if (!matchesOperandClass(operandType, rule.operandClass)) {
            continue;
        }
        return &rule;
    }
    return nullptr;
}

template <typename RuleContainer>
const typename RuleContainer::value_type *
findBinaryRule(const RuleContainer &rules, token_type token, TypeClass *leftType,
               TypeClass *rightType) {
    for (const auto &rule : rules) {
        if (rule.token != token) {
            continue;
        }
        if (!matchesOperandClass(leftType, rule.leftClass)) {
            continue;
        }
        if (!matchesOperandClass(rightType, rule.rightClass)) {
            continue;
        }
        if (rule.requireSameType &&
            !isConstQualificationConvertible(leftType,
                                             materializeValueType(nullptr, rightType)) &&
            !isConstQualificationConvertible(rightType,
                                             materializeValueType(nullptr, leftType))) {
            continue;
        }
        return &rule;
    }
    return nullptr;
}

[[noreturn]] void
errorUnsupportedBinary(token_type token, TypeClass *leftType, TypeClass *rightType,
                       const location &loc) {
    error(loc,
          "operator `" + toStdString(symbolToStr(token)) + "` doesn't support `" +
              describeType(leftType) + "` and `" + describeType(rightType) + "`",
          "Use operands with a built-in matching operator implementation, or add an explicit helper function until operator overloading is implemented.");
}

[[noreturn]] void
errorUnsupportedUnary(token_type token, TypeClass *operandType, const location &loc) {
    error(loc,
          "operator `" + toStdString(symbolToStr(token)) + "` doesn't support `" +
              describeType(operandType) + "`",
          "Use a compatible built-in operand type, or add an explicit helper function until operator overloading is implemented.");
}

}  // namespace

bool
isFloatType(TypeClass *type) {
    auto *base = asUnqualified<BaseType>(type);
    return base && (base->type == BaseType::F32 || base->type == BaseType::F64);
}

bool
isSignedIntegerType(TypeClass *type) {
    auto *base = asUnqualified<BaseType>(type);
    if (!base) {
        return false;
    }
    switch (base->type) {
    case BaseType::I8:
    case BaseType::I16:
    case BaseType::I32:
    case BaseType::I64:
        return true;
    default:
        return false;
    }
}

bool
isUnsignedIntegerType(TypeClass *type) {
    auto *base = asUnqualified<BaseType>(type);
    if (!base) {
        return false;
    }
    switch (base->type) {
    case BaseType::U8:
    case BaseType::U16:
    case BaseType::U32:
    case BaseType::U64:
    case BaseType::USIZE:
        return true;
    default:
        return false;
    }
}

bool
isIntegerType(TypeClass *type) {
    return isSignedIntegerType(type) || isUnsignedIntegerType(type);
}

bool
isNumericType(TypeClass *type) {
    return isIntegerType(type) || isFloatType(type);
}

bool
isTruthyScalarType(TypeClass *type) {
    return isIntegerType(type) || isFloatType(type) || isBoolStorageType(type) ||
        isPointerLikeType(type);
}

OperatorOperandClass
classifyOperatorOperand(TypeClass *type) {
    if (isSignedIntegerType(type)) {
        return OperatorOperandClass::SignedInt;
    }
    if (isUnsignedIntegerType(type)) {
        return OperatorOperandClass::UnsignedInt;
    }
    if (isFloatType(type)) {
        return OperatorOperandClass::Float;
    }
    if (isBoolStorageType(type)) {
        return OperatorOperandClass::Bool;
    }
    if (isPointerLikeType(type)) {
        return OperatorOperandClass::Pointer;
    }
    if (isTruthyScalarType(type)) {
        return OperatorOperandClass::TruthyScalar;
    }
    return OperatorOperandClass::Invalid;
}

UnaryOperatorBinding
OperatorResolver::resolveUnary(token_type op, TypeClass *operandType, bool addressable,
                               const location &loc) const {
    if (op == '!') {
        if (!isTruthyScalarType(operandType)) {
            errorUnsupportedUnary(op, operandType, loc);
        }
        return {op, UnaryOperatorKind::LogicalNot,
                classifyOperatorOperand(operandType), operandType, boolTy};
    }
    if (op == '~') {
        if (!isIntegerType(operandType)) {
            errorUnsupportedUnary(op, operandType, loc);
        }
        return {op, UnaryOperatorKind::BitwiseNot,
                classifyOperatorOperand(operandType), operandType,
                materializeValueType(typeTable_, operandType)};
    }
    if (op == '&') {
        if (!addressable || !operandType) {
            error(loc, "address-of expects an addressable value");
        }
        return {op, UnaryOperatorKind::AddressOf,
                classifyOperatorOperand(operandType), operandType,
                typeTable_ ? typeTable_->createPointerType(operandType) : nullptr};
    }
    if (op == '*') {
        auto *pointerType = asUnqualified<PointerType>(operandType);
        if (!pointerType) {
            error(loc, "dereference expects a pointer value");
        }
        return {op, UnaryOperatorKind::Dereference,
                OperatorOperandClass::Pointer, operandType,
                pointerType->getPointeeType()};
    }

    if (const auto *rule = findUnaryRule(op, operandType)) {
        return {op, rule->kind, rule->operandClass, operandType,
                rule->resultIsBool ? boolTy
                                   : materializeValueType(typeTable_, operandType)};
    }

    errorUnsupportedUnary(op, operandType, loc);
}

BinaryOperatorBinding
OperatorResolver::resolveBinary(token_type op, TypeClass *leftType,
                                TypeClass *rightType,
                                const location &loc) const {
    if (const auto *rule = findBinaryRule(kBinaryRules, op, leftType, rightType)) {
        return {op, rule->kind, rule->leftClass, rule->rightClass, leftType, rightType,
                rule->resultIsBool ? boolTy
                                   : materializeValueType(typeTable_, leftType),
                rule->shortCircuit};
    }
    if (const auto *rule = findBinaryRule(kExtendedBinaryRules, op, leftType, rightType)) {
        return {op, rule->kind, rule->leftClass, rule->rightClass, leftType, rightType,
                rule->resultIsBool ? boolTy
                                   : materializeValueType(typeTable_, leftType),
                rule->shortCircuit};
    }
    if (const auto *rule = findBinaryRule(kNotEqualNumericRules, op, leftType, rightType)) {
        return {op, rule->kind, rule->leftClass, rule->rightClass, leftType, rightType,
                rule->resultIsBool ? boolTy
                                   : materializeValueType(typeTable_, leftType),
                rule->shortCircuit};
    }
    if (const auto *rule = findBinaryRule(kLogicalOrRule, op, leftType, rightType)) {
        return {op, rule->kind, rule->leftClass, rule->rightClass, leftType, rightType,
                boolTy, rule->shortCircuit};
    }

    errorUnsupportedBinary(op, leftType, rightType, loc);
}

}  // namespace lona
