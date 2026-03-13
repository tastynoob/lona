#include "../ast/token.hh"
#include "parser.hh"

#include <sstream>

namespace lona {

std::ostream &
operator<<(std::ostream &os, const AstToken &token) {
    token.toString(os);
    return os;
}

string
strEscape(const string &str) {
    // "\\n" -> "\n"
    std::stringstream ss;
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '\\') {
            if (i + 1 < str.size()) {
                switch (str[i + 1]) {
                    case 'n':
                        ss << '\n';
                        break;
                    case 't':
                        ss << '\t';
                        break;
                    case 'r':
                        ss << '\r';
                        break;
                    case '0':
                        ss << '\0';
                        break;
                    case '\\':
                        ss << '\\';
                        break;
                    case '\'':
                        ss << '\'';
                        break;
                    case '\"':
                        ss << '\"';
                        break;
                    default:
                        ss << str[i];
                        break;
                }
                i++;
            } else {
                ss << str[i];
            }
        } else {
            ss << str[i];
        }
    }
    auto escaped = ss.str();
    return string(escaped.c_str());
}

int
strToSymbol(const char *str) {
    if (strcmp(str, "+") == 0) return '+';
    if (strcmp(str, "-") == 0) return '-';
    if (strcmp(str, "*") == 0) return '*';
    if (strcmp(str, "/") == 0) return '/';
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
        case Parser::token::LOGIC_AND:
            return "&&";
        case Parser::token::LOGIC_OR:
            return "||";
        default:
            return "Unknown";
    }
}

}  // namespace lona