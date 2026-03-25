#include "astnode.hh"
#include "../visitor.hh"
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <stdexcept>

namespace lona {
namespace {

std::string
tokenText(const AstToken &token) {
    return std::string(token.text.tochara(), token.text.size());
}

std::string
toStdString(const string &value) {
    return std::string(value.tochara(), value.size());
}

[[noreturn]] void
errorInvalidNumericLiteral(const AstToken &token, const std::string &message,
                          const std::string &help) {
    throw DiagnosticError(DiagnosticError::Category::Lexical, token.loc,
                          message, help);
}

struct NumericSuffixSpec {
    std::string_view name;
    AstConst::Type type;
};

constexpr NumericSuffixSpec kNumericSuffixes[] = {
    {"uint", AstConst::Type::U32},
    {"int", AstConst::Type::I32},
    {"usize", AstConst::Type::USIZE},
    {"u64", AstConst::Type::U64},
    {"i64", AstConst::Type::I64},
    {"u32", AstConst::Type::U32},
    {"i32", AstConst::Type::I32},
    {"u16", AstConst::Type::U16},
    {"i16", AstConst::Type::I16},
    {"u8", AstConst::Type::U8},
    {"i8", AstConst::Type::I8},
    {"f64", AstConst::Type::F64},
    {"f32", AstConst::Type::F32},
};

struct ParsedNumericLiteral {
    AstConst::Type type = AstConst::Type::I32;
    bool explicitType = false;
    bool hasDot = false;
    std::uint64_t integerValue = 0;
    double floatValue = 0.0;
};

std::string
removeDigitSeparators(std::string_view text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    for (char ch : text) {
        if (ch != '_') {
            cleaned.push_back(ch);
        }
    }
    return cleaned;
}

bool
parseUnsignedMagnitude(std::string_view digits, int base, std::uint64_t &value) {
    auto cleaned = removeDigitSeparators(digits);
    if (cleaned.empty()) {
        return false;
    }
    const char *begin = cleaned.data();
    const char *end = begin + cleaned.size();
    auto result = std::from_chars(begin, end, value, base);
    return result.ec == std::errc() && result.ptr == end;
}

template<typename T>
bool
fitsSignedMagnitude(std::uint64_t value) {
    return value <=
        static_cast<std::uint64_t>(std::numeric_limits<T>::max()) + 1ULL;
}

template<typename T>
bool
fitsUnsigned(std::uint64_t value) {
    return value <= std::numeric_limits<T>::max();
}

ParsedNumericLiteral
parseNumericLiteralToken(const AstToken &token) {
    auto rawText = tokenText(token);
    std::string_view raw(rawText);

    AstConst::Type suffixType = AstConst::Type::I32;
    bool hasExplicitType = false;
    for (const auto &suffix : kNumericSuffixes) {
        if (raw.size() <= suffix.name.size() + 1) {
            continue;
        }
        if (raw[raw.size() - suffix.name.size() - 1] != '_') {
            continue;
        }
        if (raw.substr(raw.size() - suffix.name.size()) != suffix.name) {
            continue;
        }
        hasExplicitType = true;
        suffixType = suffix.type;
        raw = raw.substr(0, raw.size() - suffix.name.size() - 1);
        break;
    }

    const bool hasDot = raw.find('.') != std::string_view::npos;
    ParsedNumericLiteral literal;
    literal.explicitType = hasExplicitType;
    literal.hasDot = hasDot;

    if (hasDot) {
        if (raw.find("0b") == 0 || raw.find("0o") == 0 || raw.find("0x") == 0) {
            errorInvalidNumericLiteral(
                token,
                "floating-point literals do not support base prefixes",
                "Use decimal floating-point syntax like `1.5`, or remove the decimal point for integer literals such as `0x10_u64`.");
        }
        if (hasExplicitType &&
            suffixType != AstConst::Type::F32 &&
            suffixType != AstConst::Type::F64) {
            errorInvalidNumericLiteral(
                token,
                "floating-point literal cannot use integer suffix `" +
                    std::string(rawText.substr(rawText.rfind('_') + 1)) + "`",
                "Use `_f32` or `_f64`, or remove the decimal point and cast explicitly if you need an integer value.");
        }

        auto cleaned = removeDigitSeparators(raw);
        char *end = nullptr;
        const double value = std::strtod(cleaned.c_str(), &end);
        if (!end || *end != '\0') {
            errorInvalidNumericLiteral(
                token,
                "invalid floating-point literal `" + rawText + "`",
                "Use digits with optional `_` separators, and optional `_f32` or `_f64` suffixes.");
        }

        literal.type = hasExplicitType ? suffixType : AstConst::Type::F64;
        literal.floatValue = value;
        return literal;
    }

    int base = 10;
    std::string_view digits = raw;
    if (raw.size() >= 2 && raw[0] == '0') {
        switch (raw[1]) {
        case 'b':
            base = 2;
            digits.remove_prefix(2);
            break;
        case 'o':
            base = 8;
            digits.remove_prefix(2);
            break;
        case 'x':
            base = 16;
            digits.remove_prefix(2);
            break;
        default:
            break;
        }
    }

    std::uint64_t magnitude = 0;
    if (!parseUnsignedMagnitude(digits, base, magnitude)) {
        errorInvalidNumericLiteral(
            token,
            "invalid numeric literal `" + rawText + "`",
            "Use digits that match the selected base, optional `_` separators, and a suffix like `_u64` only when needed.");
    }

    literal.integerValue = magnitude;
    literal.type = hasExplicitType ? suffixType : AstConst::Type::I32;
    if (literal.type == AstConst::Type::F32 ||
        literal.type == AstConst::Type::F64) {
        literal.floatValue = static_cast<double>(magnitude);
    }
    return literal;
}

}  // namespace

FuncPtrTypeNode*
findFuncPtrTypeNode(TypeNode *node) {
    if (node == nullptr) {
        return nullptr;
    }
    if (auto *param = dynamic_cast<FuncParamTypeNode *>(node)) {
        return findFuncPtrTypeNode(param->type);
    }
    if (auto *func = dynamic_cast<FuncPtrTypeNode *>(node)) {
        return func;
    }
    if (auto *qualified = dynamic_cast<ConstTypeNode *>(node)) {
        return findFuncPtrTypeNode(qualified->base);
    }
    if (auto *pointer = dynamic_cast<PointerTypeNode *>(node)) {
        return findFuncPtrTypeNode(pointer->base);
    }
    if (auto *indexable = dynamic_cast<IndexablePointerTypeNode *>(node)) {
        return findFuncPtrTypeNode(indexable->base);
    }
    if (auto *array = dynamic_cast<ArrayTypeNode *>(node)) {
        return findFuncPtrTypeNode(array->base);
    }
    return nullptr;
}

TypeNode*
createPointerOrArrayTypeNode(TypeNode *head, std::vector<AstNode *> *suffix) {
    if (suffix == nullptr || suffix->empty()) {
        return head;
    }

    TypeNode *node = head;
    for (auto *it : *suffix) {
        if (it == nullptr) {
            node = new PointerTypeNode(node, 1, node ? node->loc : location());
            continue;
        }
        if (it == reinterpret_cast<AstNode *>(1)) {
            node = new ArrayTypeNode(node, {}, node ? node->loc : location());
            continue;
        }
        node = new ArrayTypeNode(node, std::vector<AstNode *>{it},
                                 node ? node->loc : location());
    }
    return node;
}

Object *
AstNode::accept(AstVisitor &) {
    throw std::runtime_error("Cannot visit abstract AstNode directly");
}

#define DEF_ACCEPT(classname)                \
    Object *classname::accept(AstVisitor &visitor) { \
        return visitor.visit(this);          \
    }

DEF_ACCEPT(AstProgram)
DEF_ACCEPT(AstTagNode)
DEF_ACCEPT(AstStatList)
DEF_ACCEPT(AstConst)
DEF_ACCEPT(AstField)
DEF_ACCEPT(AstFuncRef)
DEF_ACCEPT(AstAssign)
DEF_ACCEPT(AstBinOper)
DEF_ACCEPT(AstUnaryOper)
DEF_ACCEPT(AstRefExpr)
DEF_ACCEPT(AstTupleLiteral)
DEF_ACCEPT(AstBraceInitItem)
DEF_ACCEPT(AstBraceInit)
DEF_ACCEPT(AstNamedCallArg)
DEF_ACCEPT(AstStructDecl)
DEF_ACCEPT(AstImport)
DEF_ACCEPT(AstVarDecl)
DEF_ACCEPT(AstVarDef)
DEF_ACCEPT(AstFuncDecl)
DEF_ACCEPT(AstRet)
DEF_ACCEPT(AstBreak)
DEF_ACCEPT(AstContinue)
DEF_ACCEPT(AstIf)
DEF_ACCEPT(AstFor)
DEF_ACCEPT(AstCastExpr)
DEF_ACCEPT(AstSizeofExpr)
DEF_ACCEPT(AstFieldCall)
DEF_ACCEPT(AstDotLike)

AstProgram::AstProgram(AstNode *body)
    : AstNode(body ? body->loc : location()), body(body->as<AstStatList>()) {
    assert(body->is<AstStatList>());
}

AstConst::AstConst(AstToken &token) : AstNode(token.loc) {
    switch (token.type) {
        case TokenType::ConstNumeric: {
            auto literal = parseNumericLiteralToken(token);
            switch (literal.type) {
            case Type::I8:
                if (!fitsSignedMagnitude<std::int8_t>(literal.integerValue)) {
                    errorInvalidNumericLiteral(
                        token,
                        "integer literal `" + tokenText(token) +
                            "` is out of range for `i8`",
                        "Use a wider integer suffix like `_i16`, or write a smaller value.");
                }
                if (literal.integerValue ==
                    static_cast<std::uint64_t>(std::numeric_limits<std::int8_t>::max()) + 1ULL) {
                    setNumericLiteral(Type::I8, literal.explicitType,
                                      new std::uint64_t(literal.integerValue), true);
                } else {
                    setNumericLiteral(Type::I8, literal.explicitType,
                                      new std::int8_t(static_cast<std::int8_t>(literal.integerValue)));
                }
                break;
            case Type::U8:
                if (!fitsUnsigned<std::uint8_t>(literal.integerValue)) {
                    errorInvalidNumericLiteral(
                        token,
                        "integer literal `" + tokenText(token) +
                            "` is out of range for `u8`",
                        "Use a wider integer suffix like `_u16`, or write a smaller value.");
                }
                setNumericLiteral(Type::U8, literal.explicitType,
                                  new std::uint8_t(static_cast<std::uint8_t>(literal.integerValue)));
                break;
            case Type::I16:
                if (!fitsSignedMagnitude<std::int16_t>(literal.integerValue)) {
                    errorInvalidNumericLiteral(
                        token,
                        "integer literal `" + tokenText(token) +
                            "` is out of range for `i16`",
                        "Use a wider integer suffix like `_i32`, or write a smaller value.");
                }
                if (literal.integerValue ==
                    static_cast<std::uint64_t>(std::numeric_limits<std::int16_t>::max()) + 1ULL) {
                    setNumericLiteral(Type::I16, literal.explicitType,
                                      new std::uint64_t(literal.integerValue), true);
                } else {
                    setNumericLiteral(Type::I16, literal.explicitType,
                                      new std::int16_t(static_cast<std::int16_t>(literal.integerValue)));
                }
                break;
            case Type::U16:
                if (!fitsUnsigned<std::uint16_t>(literal.integerValue)) {
                    errorInvalidNumericLiteral(
                        token,
                        "integer literal `" + tokenText(token) +
                            "` is out of range for `u16`",
                        "Use a wider integer suffix like `_u32`, or write a smaller value.");
                }
                setNumericLiteral(Type::U16, literal.explicitType,
                                  new std::uint16_t(static_cast<std::uint16_t>(literal.integerValue)));
                break;
            case Type::I32:
                if (!fitsSignedMagnitude<std::int32_t>(literal.integerValue)) {
                    errorInvalidNumericLiteral(
                        token,
                        "integer literal `" + tokenText(token) +
                            "` is out of range for `i32`",
                        "Use a wider suffix like `_u64` or `_i64` if you need a larger integer literal.");
                }
                if (literal.integerValue ==
                    static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1ULL) {
                    setNumericLiteral(Type::I32, literal.explicitType,
                                      new std::uint64_t(literal.integerValue), true);
                } else {
                    setNumericLiteral(Type::I32, literal.explicitType,
                                      new std::int32_t(static_cast<std::int32_t>(literal.integerValue)));
                }
                break;
            case Type::U32:
                if (!fitsUnsigned<std::uint32_t>(literal.integerValue)) {
                    errorInvalidNumericLiteral(
                        token,
                        "integer literal `" + tokenText(token) +
                            "` is out of range for `u32`",
                        "Use a wider integer suffix like `_u64` if you need a larger value.");
                }
                setNumericLiteral(Type::U32, literal.explicitType,
                                  new std::uint32_t(static_cast<std::uint32_t>(literal.integerValue)));
                break;
            case Type::I64:
                if (!fitsSignedMagnitude<std::int64_t>(literal.integerValue)) {
                    errorInvalidNumericLiteral(
                        token,
                        "integer literal `" + tokenText(token) +
                            "` is out of range for `i64`",
                        "Use an unsigned suffix like `_u64`, or write a smaller signed value.");
                }
                if (literal.integerValue ==
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL) {
                    setNumericLiteral(Type::I64, literal.explicitType,
                                      new std::uint64_t(literal.integerValue), true);
                } else {
                    setNumericLiteral(Type::I64, literal.explicitType,
                                      new std::int64_t(static_cast<std::int64_t>(literal.integerValue)));
                }
                break;
            case Type::U64:
                setNumericLiteral(Type::U64, literal.explicitType,
                                  new std::uint64_t(literal.integerValue));
                break;
            case Type::USIZE:
                setNumericLiteral(Type::USIZE, literal.explicitType,
                                  new std::uint64_t(literal.integerValue));
                break;
            case Type::F32:
                setNumericLiteral(Type::F32, literal.explicitType,
                                  new float(static_cast<float>(literal.floatValue)));
                break;
            case Type::F64:
                setNumericLiteral(Type::F64, literal.explicitType,
                                  new double(literal.floatValue));
                break;
            default:
                throw std::runtime_error("Invalid parsed numeric literal type");
            }
            break;
        }
        case TokenType::ConstInt32:
            this->vtype = Type::I32;
            this->buf = (char *)new int32_t(token.text.toI32());
            break;
        case TokenType::ConstFP64:
            this->vtype = Type::F64;
            this->buf = (char *)new double(std::stod(token.text.tochara()));
            break;
        case TokenType::ConstStr:
            this->vtype = Type::STRING;
            this->buf = new string(token.text);
            break;
        case TokenType::ConstChar:
            this->vtype = Type::CHAR;
            this->buf = new string(token.text);
            break;
        case TokenType::ConstBool:
            this->vtype = Type::BOOL;
            this->buf = (char *)new bool(std::strcmp(token.text.tochara(), "true") == 0);
            break;
        case TokenType::ConstNull:
            this->vtype = Type::NULLPTR;
            this->buf = nullptr;
            break;
        default:
            throw std::runtime_error("Invalid token type for AstConst");
    }
}

void
AstConst::setNumericLiteral(Type type, bool explicitType, void *value,
                            bool unaryMinusOnly) {
    this->vtype = type;
    this->explicitNumericType = explicitType;
    this->requiresUnaryMinusForSignedMin = unaryMinusOnly;
    this->buf = value;
}

AstField::AstField(AstToken &token) : AstNode(token.loc), name(token.text) {
    assert(token.type == TokenType::Field);
}

AstAssign::AstAssign(AstNode *left, AstNode *right)
    : AstNode(left ? left->loc : (right ? right->loc : location())),
      left(left), right(right) {}

AstBinOper::AstBinOper(AstNode *left, token_type op, AstNode *right)
    : AstNode(left ? left->loc : (right ? right->loc : location())),
      left(left), op(op), right(right) {}

AstUnaryOper::AstUnaryOper(token_type op, AstNode *expr)
    : AstNode(expr ? expr->loc : location()), op(op), expr(expr) {}

AstVarDecl::AstVarDecl(BindingKind bindingKind, AstToken &field,
                       TypeNode *typeNode, AstNode *right)
    : AstNode(field.loc),
      bindingKind(bindingKind),
      field(field.text),
      typeNode(typeNode),
      right(right) {}

void
AstStatList::push(AstNode *node) {
    this->body.push_back(node);
}

AstStatList::AstStatList(AstNode *node)
    : AstNode(node ? node->loc : location()) {
    this->body.push_back(node);
}

AstFuncDecl::AstFuncDecl(AstToken &name, AstNode *body,
                         std::vector<AstNode *> *args, TypeNode *retType,
                         AbiKind abiKind)
    : AstNode(name.loc),
      name(name.text),
      args(args),
      body(body),
      retType(retType),
      abiKind(abiKind) {}

AstRet::AstRet(const location &loc, AstNode *expr) : AstNode(loc), expr(expr) {}

AstIf::AstIf(AstNode *condition, AstNode *then, AstNode *els)
    : AstNode(condition ? condition->loc : location()),
      condition(condition), then(then), els(els) {}

AstFor::AstFor(AstNode *expr, AstNode *body, AstNode *els)
    : AstNode(expr ? expr->loc : location()), expr(expr), body(body), els(els) {}

AstFieldCall::AstFieldCall(AstNode *value, std::vector<AstNode *> *args)
    : AstNode(value ? value->loc : location()), value(value), args(args) {}

std::string
describeDotLikeSyntax(const AstNode *node, std::string_view nullDescription) {
    if (!node) {
        return std::string(nullDescription);
    }
    if (auto *field = dynamic_cast<const AstField *>(node)) {
        return toStdString(field->name);
    }
    if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
        auto parent = describeDotLikeSyntax(dotLike->parent, nullDescription);
        auto fieldName = tokenText(*dotLike->field);
        if (parent.empty()) {
            return fieldName;
        }
        return parent + "." + fieldName;
    }
    return std::string(nullDescription);
}

bool
collectDotLikeSegments(const AstNode *node, std::vector<std::string> &segments) {
    if (!node) {
        return false;
    }
    if (auto *field = dynamic_cast<const AstField *>(node)) {
        segments.push_back(toStdString(field->name));
        return true;
    }
    if (auto *dotLike = dynamic_cast<const AstDotLike *>(node)) {
        if (!collectDotLikeSegments(dotLike->parent, segments)) {
            return false;
        }
        segments.push_back(tokenText(*dotLike->field));
        return true;
    }
    return false;
}

}  // namespace lona
