#include "ast/token.hh"

#include <sstream>

namespace lona {

std::ostream &
operator<<(std::ostream &os, const AstToken &token) {
    token.toString(os);
    return os;
}

std::string
strEscape(const std::string &str) {
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
    return ss.str();
}

}  // namespace lona