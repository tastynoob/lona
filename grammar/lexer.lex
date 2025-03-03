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

(true) { loc->columns(yyleng); return token::TRUE; }
(false) { loc->columns(yyleng); return token::FALSE; }
(def) { loc->columns(yyleng); return token::DEF; }
(ret) { lval->token = new AstToken(*loc); loc->columns(yyleng); return token::RET; }
(if) { loc->columns(yyleng); return token::IF; }
(else) { loc->columns(yyleng); return token::ELSE; }
(for) { loc->columns(yyleng); return token::FOR; }
(struct) { loc->columns(yyleng); return token::STRUCT; }
(case) {}
(pass) {}
(cast) {}
(class) {}

[0-9]+ {
    lval->token = new AstToken(TokenType::ConstInt32, yytext, *loc);
    loc->columns(yyleng);
    return token::CONST;
}

[0-9]+\.[0-9]+ {
    lval->token = new AstToken(TokenType::ConstFP32, yytext, *loc);
    loc->columns(yyleng);
    return token::CONST;
}

(\"[^\"]*\") {
    char* strpos = yytext + 1;
    yytext[yyleng - 1] = '\0';
    lval->token = new AstToken(TokenType::ConstStr, strEscape(std::string(strpos)).c_str(), *loc);
    loc->columns(yyleng);
    return token::CONST;
}

[a-zA-Z_][a-zA-Z0-9_]* {
    lval->token = new AstToken(TokenType::Field, yytext, *loc);
    loc->columns(yyleng);
    return token::FIELD;
}

(\+|-|\*|\/|!|~|<|>|\||&|^) {
    // + - * / ! ~ < > | & ^
    loc->columns(yyleng);
    return yytext[0];
}

(\+=) {
    loc->columns(yyleng);
    return token::ASSIGN_ADD;
}

(-=) {
    loc->columns(yyleng);
    return token::ASSIGN_SUB;
}

(==) {
    loc->columns(yyleng);
    return token::LOGIC_EQUAL;
}

(!=) {
    loc->columns(yyleng);
    return token::LOGIC_NOT_EQUAL;
}

(&&) {
    loc->columns(yyleng);
    return token::LOGIC_AND;
}

(\|\|) {
    // ||
    loc->columns(yyleng);
    return token::LOGIC_OR;
}

(\{|\}) {
    // { }
    skip_semi = true;
    loc->columns(yyleng);
    return yytext[0];
}

(:|=|\(|\)|\[|\]|@|#|,|\.) {
    // : = ( ) [ ] @ # , .
    loc->columns(yyleng);
    return yytext[0];
}

([ ]*\n[ \n]*) {
    int line_num = 0;
    int space_num = 0;
    for (int i = 0; i < yyleng; i++) {
        if (yytext[i] == '\n') {
            line_num++;
            space_num = 0;
        }
        if (yytext[i] == ' ') {
            space_num++;
        }
    }
    loc->lines(line_num);
    loc->columns(space_num);
    if (skip_semi) {
        skip_semi = false;
        break;
    }
    return token::NEWLINE;
}

\/\/[^\n]* {
    loc->lines(1);
}
[ \t]+ {
    loc->columns(yyleng);
}

.|\n {
    std::cerr << "Unrecognized token: \'" << yytext << "\'" << std::endl;
    return EOF;
}
%%