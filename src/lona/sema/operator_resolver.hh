#pragma once

#include "lona/ast/astnode.hh"
#include "lona/type/type.hh"

namespace lona {

class TypeTable;

enum class OperatorOperandClass {
    Invalid,
    SignedInt,
    UnsignedInt,
    Float,
    Bool,
    Pointer,
    TruthyScalar,
};

enum class UnaryOperatorKind {
    Identity,
    Negate,
    LogicalNot,
    BitwiseNot,
    AddressOf,
    Dereference,
};

enum class BinaryOperatorKind {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    ShiftLeft,
    ShiftRight,
    BitAnd,
    BitXor,
    BitOr,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    Equal,
    NotEqual,
    LogicalAnd,
    LogicalOr,
};

struct UnaryOperatorBinding {
    token_type token = 0;
    UnaryOperatorKind kind = UnaryOperatorKind::Identity;
    OperatorOperandClass operandClass = OperatorOperandClass::Invalid;
    TypeClass *operandType = nullptr;
    TypeClass *resultType = nullptr;
};

struct BinaryOperatorBinding {
    token_type token = 0;
    BinaryOperatorKind kind = BinaryOperatorKind::Add;
    OperatorOperandClass leftClass = OperatorOperandClass::Invalid;
    OperatorOperandClass rightClass = OperatorOperandClass::Invalid;
    TypeClass *leftType = nullptr;
    TypeClass *rightType = nullptr;
    TypeClass *resultType = nullptr;
    bool shortCircuit = false;
};

bool isFloatType(TypeClass *type);
bool isSignedIntegerType(TypeClass *type);
bool isUnsignedIntegerType(TypeClass *type);
bool isIntegerType(TypeClass *type);
bool isNumericType(TypeClass *type);
bool isTruthyScalarType(TypeClass *type);
OperatorOperandClass classifyOperatorOperand(TypeClass *type);

class OperatorResolver {
    TypeTable *typeTable_ = nullptr;

public:
    explicit OperatorResolver(TypeTable *typeTable) : typeTable_(typeTable) {}

    UnaryOperatorBinding resolveUnary(token_type op, TypeClass *operandType,
                                      bool addressable,
                                      const location &loc) const;
    BinaryOperatorBinding resolveBinary(token_type op, TypeClass *leftType,
                                        TypeClass *rightType,
                                        const location &loc) const;
};

}  // namespace lona
