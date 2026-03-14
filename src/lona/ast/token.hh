#pragma once

#include <iostream>
#include <stdexcept>
#include <string.h>
#include <string>

#include "location.hh"
#include "../util/string.hh"


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
    ConstBool,
};

inline const char *
tokenTypeToStr(TokenType type) {
    switch (type) {
        case TokenType::ConstInt32:
        case TokenType::ConstFP32:
        case TokenType::ConstStr:
        case TokenType::ConstBool:
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
public:
    TokenType const type = TokenType::Invalid;
    string const text;
    location const loc;
    AstToken() {}
    AstToken(location loc) : loc(loc) {}
    AstToken(TokenType type, const char *text, location loc)
        : type(type), text(text), loc(loc) {}
    void toString(std::ostream &os) const {
        os << "AstToken(" << tokenTypeToStr(type) << ", " << text << ")";
    }

    const int toInt() {
        return text.toI32();
    }
};

std::ostream &
operator<<(std::ostream &os, const AstToken &token);

int
strToSymbol(const char *str);

string
symbolToStr(int symbol);

string
strEscape(const string &str);

}  // namespace lona