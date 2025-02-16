%{
#include "ast/token.hh"
#include "scan/scanner.hh"

#undef YY_DECL
#define YY_DECL int lona::Scanner::yylex(Parser::semantic_type* const lval, Parser::location_type* loc)

#define yyterminate() return 0

using token = lona::Parser::token::token_kind_type;

#define YY_USER_ACTION loc->step(); loc->columns(yyleng);
%}

%option yyclass="lona::Scanner"
%option noyywrap
%option nodefault
%option c++


%%

(true) { return token::TRUE; }
(false) { return token::FALSE; }
(def) { return token::DEF; }
(ret) { return token::RET; }
(if) { return token::IF; }
(else) { return token::ELSE; }
(for) { return token::FOR; }
(struct) { return token::STRUCT; }
(case) {}
(pass) {}
(cast) {}
(class) {}

[0-9]+ {
    lval->token = new AstToken(TokenType::ConstInt32, yytext, *loc);
    return token::CONST;
}

[0-9]+\.[0-9]+ {
    lval->token = new AstToken(TokenType::ConstFP32, yytext, *loc);
    return token::CONST;
}

(\"[^\"]*\") {
    char* strpos = yytext + 1;
    yytext[yyleng - 1] = '\0';
    lval->token = new AstToken(TokenType::ConstStr, strEscape(std::string(strpos)).c_str(), *loc);
    return token::CONST;
}

[a-zA-Z_][a-zA-Z0-9_]* {
    lval->token = new AstToken(TokenType::Field, yytext, *loc);
    return token::FIELD;
}

(\+|-|\*|\/|!|~|<|>|\||&|^) {
    // + - * / ! ~ < > | & ^
    return yytext[0];
}

(==) {
    return token::LOGIC_EQUAL;
}

(&&) {
    return token::LOGIC_AND;
}

(\|\|) {
    return token::LOGIC_OR;
}

(\{|\}) {
    // { }
    skip_semi = true;
    return yytext[0];
}

(:|=|\(|\)|\[|\]|@|#|,|\.) {
    // : = ( ) [ ] @ # , .
    return yytext[0];
}

([ ]*\n[ \n]*) {
    /* count \n */
    int count = 0;
    for (int i = 0; i < yyleng; i++) {
        if (yytext[i] == '\n') {
            count++;
        }
    }
    loc->lines(count);
    if (skip_semi) {
        skip_semi = false;
        break;
    }
    return token::NEWLINE;
}

\/\/[^\n]* { }
[ \t]+ { /* ignore */ }

.|\n {
    std::cerr << "Unrecognized token: \'" << yytext << "\'" << std::endl;
    return EOF;
}
%%