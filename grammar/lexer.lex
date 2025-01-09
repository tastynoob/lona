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
(case) {}
(pass) {}
(cast) {}
(class) {}

[0-9]+ {
    lval->build<AstToken>(AstToken(TokenType::ConstInt32, yytext, 0, 0));
    return token::CONST;
}

[0-9]+\.[0-9]+ {
    lval->build<AstToken>(AstToken(TokenType::ConstFP32, yytext, 0, 0));
    return token::CONST;
}

(\"[^\"]*\") {
    char* strpos = yytext + 1;
    yytext[yyleng - 1] = '\0';
    lval->build<AstToken>(AstToken(TokenType::ConstStr, strEscape(std::string(strpos)).c_str(), 0, 0));
    return token::CONST;
}

[a-zA-Z_][a-zA-Z0-9_]* {
    lval->build<AstToken>(AstToken(TokenType::Field, yytext, 0, 0));
    return token::FIELD;
}

(\*|\/) {
    lval->build<AstToken>(AstToken(TokenType::Operlv0, yytext, 0, 0));
    return token::OPERLV0;
}

(\+|-) {
    lval->build<AstToken>(AstToken(TokenType::Operlv1, yytext, 0, 0));
    return token::OPERLV1;
}

(<|>|<=|>=|==|!=) {    lval->build<AstToken>(AstToken(TokenType::Operlv2, yytext, 0, 0));
    return token::OPERLV2;
}

(!|~) {
    lval->build<AstToken>(AstToken(TokenType::OperUnary, yytext, 0, 0));
    return token::OPERUNARY;
}

(:|=|\(|\)|\[|\]|\{|\}|<|>|@|#|,|\.) { return yytext[0]; } /* : = ( ) [ ] { } <> @ # , . */
     
([ ]*\n[ \n]*) {
    /* count \n */
    int count = 0;
    for (int i = 0; i < yyleng; i++) {
        if (yytext[i] == '\n') {
            count++;
        }
    }
    loc->lines(count);
    return token::NEWLINE;
}

[ \t]+ { /* ignore */ }

.|\n {
    std::cerr << "Unrecognized token: \'" << yytext << "\'" << std::endl;
    return EOF;
}
%%