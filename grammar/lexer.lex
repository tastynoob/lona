%{
#include "lona/err/err.hh"
#include "lona/ast/token.hh"
#include "lona/scan/scanner.hh"

#undef YY_DECL
#define YY_DECL int lona::Scanner::yylex(Parser::semantic_type* const lval, Parser::location_type* loc)

#define yyterminate() return 0

using token = lona::Parser::token::token_kind_type;

#define YY_USER_ACTION loc->step();

static std::string describeLexeme(const char *text, int length) {
    if (length == 1 && text[0] == '\n') {
        return "\\n";
    }
    if (length == 1 && text[0] == '\t') {
        return "\\t";
    }
    return std::string(text, text + length);
}
%}

%option yyclass="lona::Scanner"
%option noyywrap
%option nodefault
%option c++
%x IMPORT_PATH_STATE


%%

(true) { loc->columns(yyleng); return token::TRUE; }
(false) { loc->columns(yyleng); return token::FALSE; }
(var) { loc->columns(yyleng); return token::VAR; }

(def) { loc->columns(yyleng); return token::DEF; }
(import) { loc->columns(yyleng); BEGIN(IMPORT_PATH_STATE); return token::IMPORT; }
(ret) { loc->columns(yyleng); lval->token = new AstToken(*loc); return token::RET; }

(if) { loc->columns(yyleng); return token::IF; }
(else) { loc->columns(yyleng); return token::ELSE; }
(for) { loc->columns(yyleng); return token::FOR; }

(struct) { loc->columns(yyleng); return token::STRUCT; }
(type|u8|i8|u16|i16|u32|i32|u64|i64|int|uint|f32|f64|bool|str) {
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::Field, yytext, *loc);
    return token::TYPE;
}
(class) {}
(trait) {}
(case) {}
(cast) {}


<IMPORT_PATH_STATE>[ \t]+ {
    loc->columns(yyleng);
}

<IMPORT_PATH_STATE>[A-Za-z0-9_./-]+ {
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::Field, yytext, *loc);
    BEGIN(INITIAL);
    return token::IMPORT_PATH;
}

<IMPORT_PATH_STATE>([ ]*\n[ \n]*) {
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
    BEGIN(INITIAL);
    return token::NEWLINE;
}

<IMPORT_PATH_STATE>.|\n {
    loc->columns(yyleng);
    throw lona::DiagnosticError(
        lona::DiagnosticError::Category::Lexical, *loc,
        "I couldn't recognize this import path token.",
        "Write imports like `import path/to/file` without quotes or file suffix.");
}

[0-9]+ {
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::ConstInt32, yytext, *loc);
    return token::CONST;
}

[0-9]+\.[0-9]+ {
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::ConstFP32, yytext, *loc);
    return token::CONST;
}

(\"[^\"]*\") {
    char* strpos = yytext + 1;
    yytext[yyleng - 1] = '\0';
    string escaped = strEscape(string(strpos));
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::ConstStr, escaped.tochara(), *loc);
    return token::CONST;
}

[a-zA-Z_][a-zA-Z0-9_]* {
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::Field, yytext, *loc);
    return token::FIELD;
}

(&<) {
    loc->columns(yyleng);
    return token::FUNC_PTR_OPEN;
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
    loc->columns(yyleng);
}
[ \t]+ {
    loc->columns(yyleng);
}

.|\n {
    loc->columns(yyleng);
    throw lona::DiagnosticError(
        lona::DiagnosticError::Category::Lexical, *loc,
        "I couldn't recognize the token `" + describeLexeme(yytext, yyleng) + "`.",
        "Check for a typo or an unsupported character here.");
}
%%
