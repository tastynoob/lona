#pragma once

#include <iostream>
#include <stdexcept>
#include <string.h>
#include <string>

namespace lona {

enum class TokenType {
    ConstInt32,
    ConstFP32,
    ConstStr,
    Operlv0,    // * /
    Operlv1,    // + -
    Operlv2,    // < > <= >= == !=
    Operlv3,    // && ||
    OperUnary,  // ! ~
    Colon,      // :
    Field,
    Invalid,
    NumType,
};

inline const char *
tokenTypeToStr(TokenType type) {
    switch (type) {
        case TokenType::ConstInt32:
        case TokenType::ConstFP32:
        case TokenType::ConstStr:
            return "Const";
        case TokenType::Operlv0:
            return "Operlv0";
        case TokenType::Operlv1:
            return "Operlv1";
        case TokenType::Operlv2:
            return "Operlv2";
        case TokenType::Operlv3:
            return "Operlv3";
        case TokenType::OperUnary:
            return "OperUnary";
        case TokenType::Colon:
            return "Colon";
        case TokenType::Field:
            return "Field";
        case TokenType::Invalid:
            return "Invalid";
        case TokenType::NumType:
            return "NumType";
        default:
            return "Unknown";
    }
}

class AstToken {
    TokenType type = TokenType::Invalid;
    std::string text;
    int row = -1, col = -1;

public:
    AstToken() {}
    AstToken(TokenType type, const char *text, int row, int col)
        : type(type), text(text), row(row), col(col) {}
    void toString(std::ostream &os) const {
        os << "AstToken(" << tokenTypeToStr(type) << ", " << text << ")";
    }

    const TokenType &getType() { return type; }
    const std::string &getText() { return text; }
    const int toInt() { return std::stoi(text); }
};

std::ostream &
operator<<(std::ostream &os, const AstToken &token);

enum class SymbolTable {
    ADD,  // +
    SUB,  // -
    MUL,  // *
    DIV,  // /
    MOD,  // %
    LT,   // <
    GT,   // >
    LE,   // <=
    GE,   // >=
    EQ,   // ==
    NE,   // !=
    AND,  // &&
    OR,   // ||
    NOT,  // !
    NEG,  // ~
    Invalid
};

SymbolTable inline strToSymbol(const char *str) {
    if (strcmp(str, "+") == 0) return SymbolTable::ADD;
    if (strcmp(str, "-") == 0) return SymbolTable::SUB;
    if (strcmp(str, "*") == 0) return SymbolTable::MUL;
    if (strcmp(str, "/") == 0) return SymbolTable::DIV;
    if (strcmp(str, "%") == 0) return SymbolTable::MOD;
    if (strcmp(str, "<") == 0) return SymbolTable::LT;
    if (strcmp(str, ">") == 0) return SymbolTable::GT;
    if (strcmp(str, "<=") == 0) return SymbolTable::LE;
    if (strcmp(str, ">=") == 0) return SymbolTable::GE;
    if (strcmp(str, "==") == 0) return SymbolTable::EQ;
    if (strcmp(str, "!=") == 0) return SymbolTable::NE;
    if (strcmp(str, "&&") == 0) return SymbolTable::AND;
    if (strcmp(str, "||") == 0) return SymbolTable::OR;
    if (strcmp(str, "!") == 0) return SymbolTable::NOT;
    if (strcmp(str, "~") == 0) return SymbolTable::NEG;
    throw std::runtime_error("Invalid symbol");
}

std::string inline symbolToStr(SymbolTable symbol) {
    switch (symbol) {
        case SymbolTable::ADD:
            return "+";
        case SymbolTable::SUB:
            return "-";
        case SymbolTable::MUL:
            return "*";
        case SymbolTable::DIV:
            return "/";
        case SymbolTable::MOD:
            return "%";
        case SymbolTable::LT:
            return "<";
        case SymbolTable::GT:
            return ">";
        case SymbolTable::LE:
            return "<=";
        case SymbolTable::GE:
            return ">=";
        default:
            return "Unknown";
    }
}

std::string
strEscape(const std::string &str);

}  // namespace lona