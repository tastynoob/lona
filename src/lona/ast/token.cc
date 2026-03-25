#include "../ast/token.hh"
#include "parser.hh"

#include <cctype>
#include <cstdint>
#include <sstream>

namespace lona {

std::ostream &
operator<<(std::ostream &os, const AstToken &token) {
    token.toString(os);
    return os;
}

string
strEscape(const string &str) {
    std::string bytes;
    bytes.reserve(str.size());
    auto appendByte = [&](unsigned value) {
        bytes.push_back(static_cast<char>(static_cast<std::uint8_t>(value)));
    };
    auto hexValue = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        return -1;
    };

    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '\\') {
            if (i + 1 < str.size()) {
                switch (str[i + 1]) {
                    case 'n':
                        appendByte('\n');
                        break;
                    case 't':
                        appendByte('\t');
                        break;
                    case 'r':
                        appendByte('\r');
                        break;
                    case '0':
                        appendByte('\0');
                        break;
                    case '\\':
                        appendByte('\\');
                        break;
                    case '\'':
                        appendByte('\'');
                        break;
                    case '\"':
                        appendByte('\"');
                        break;
                    case 'x':
                        if (i + 2 < str.size()) {
                            const int first = hexValue(str[i + 2]);
                            if (first >= 0) {
                                if (i + 3 < str.size()) {
                                    const int second = hexValue(str[i + 3]);
                                    if (second >= 0) {
                                        appendByte(static_cast<unsigned>((first << 4) | second));
                                        i += 2;
                                        break;
                                    }
                                }
                                appendByte(static_cast<unsigned>(first));
                                i += 1;
                                break;
                            }
                        }
                        appendByte('\\');
                        appendByte('x');
                        break;
                    default:
                        appendByte('\\');
                        appendByte(str[i + 1]);
                        break;
                }
                i++;
            } else {
                appendByte(str[i]);
            }
        } else {
            appendByte(str[i]);
        }
    }
    return string(bytes.data(), static_cast<uint32_t>(bytes.size()));
}

int
strToSymbol(const char *str) {
    if (strcmp(str, "+") == 0) return '+';
    if (strcmp(str, "-") == 0) return '-';
    if (strcmp(str, "*") == 0) return '*';
    if (strcmp(str, "/") == 0) return '/';
    if (strcmp(str, "%") == 0) return '%';
    if (strcmp(str, "&") == 0) return '&';
    if (strcmp(str, "^") == 0) return '^';
    if (strcmp(str, "|") == 0) return '|';
    if (strcmp(str, "!") == 0) return '!';
    if (strcmp(str, "~") == 0) return '~';
    if (strcmp(str, "<") == 0) return '<';
    if (strcmp(str, ">") == 0) return '>';
    if (strcmp(str, "==") == 0) return Parser::token::LOGIC_EQUAL;
    if (strcmp(str, "!=") == 0) return Parser::token::LOGIC_NOT_EQUAL;
    if (strcmp(str, "<=") == 0) return Parser::token::LOGIC_LE;
    if (strcmp(str, ">=") == 0) return Parser::token::LOGIC_GE;
    if (strcmp(str, "<<") == 0) return Parser::token::SHIFT_LEFT;
    if (strcmp(str, ">>") == 0) return Parser::token::SHIFT_RIGHT;
    if (strcmp(str, "&&") == 0) return Parser::token::LOGIC_AND;
    if (strcmp(str, "||") == 0) return Parser::token::LOGIC_OR;
    throw std::runtime_error("Invalid symbol");
}

string
symbolToStr(int symbol) {
    switch (symbol) {
        case '+':
            return "+";
        case '-':
            return "-";
        case '*':
            return "*";
        case '/':
            return "/";
        case '%':
            return "%";
        case '<':
            return "<";
        case '>':
            return ">";
        case '&':
            return "&";
        case '|':
            return "|";
        case '!':
            return "!";
        case '~':
            return "~";
        case Parser::token::LOGIC_EQUAL:
            return "==";
        case Parser::token::LOGIC_NOT_EQUAL:
            return "!=";
        case Parser::token::LOGIC_LE:
            return "<=";
        case Parser::token::LOGIC_GE:
            return ">=";
        case Parser::token::SHIFT_LEFT:
            return "<<";
        case Parser::token::SHIFT_RIGHT:
            return ">>";
        case Parser::token::LOGIC_AND:
            return "&&";
        case Parser::token::LOGIC_OR:
            return "||";
        default:
            return "Unknown";
    }
}

}  // namespace lona
