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

static void advanceNewlineSpan(lona::Parser::location_type *loc, const char *text,
                               int length) {
    int line_num = 0;
    int space_num = 0;
    for (int i = 0; i < length; i++) {
        if (text[i] == '\n') {
            line_num++;
            space_num = 0;
        }
        if (text[i] == ' ') {
            space_num++;
        }
    }
    loc->lines(line_num);
    loc->columns(space_num);
}

static void
throwInvalidNumericLiteral(lona::Parser::location_type *loc, const char *text,
                           int length, const std::string &help) {
    loc->columns(length);
    throw lona::DiagnosticError(
        lona::DiagnosticError::Category::Lexical, *loc,
        "I couldn't recognize this numeric literal: `" +
            describeLexeme(text, length) + "`",
        help);
}

#define RETURN_PLAIN_TOKEN(tok) \
    do {                        \
        loc->columns(yyleng);   \
        return tok;             \
    } while (0)
%}

%option yyclass="lona::Scanner"
%option noyywrap
%option nodefault
%option c++
%x IMPORT_PATH_STATE

NUM_SUFFIX (_(u8|i8|u16|i16|u32|i32|u64|i64|usize|int|uint|f32|f64))
DEC_DIGITS [0-9](_?[0-9])*
BIN_DIGITS [01](_?[01])*
OCT_DIGITS [0-7](_?[0-7])*
HEX_DIGITS [0-9A-Fa-f](_?[0-9A-Fa-f])*
DEC_FLOAT {DEC_DIGITS}\.{DEC_DIGITS}
NUMERIC_LITERAL ((0b{BIN_DIGITS}|0o{OCT_DIGITS}|0x{HEX_DIGITS}|{DEC_FLOAT}|{DEC_DIGITS}){NUM_SUFFIX}?)


%%

(true) { RETURN_PLAIN_TOKEN(token::TRUE); }
(false) { RETURN_PLAIN_TOKEN(token::FALSE); }
(null) { RETURN_PLAIN_TOKEN(token::NULL_KW); }
	(var) { RETURN_PLAIN_TOKEN(token::VAR); }
	(global) { RETURN_PLAIN_TOKEN(token::GLOBAL); }
	(ref) { RETURN_PLAIN_TOKEN(token::REF); }
	(const) { RETURN_PLAIN_TOKEN(token::TYPE_CONST); }
(cast) { RETURN_PLAIN_TOKEN(token::CAST); }
(sizeof) { RETURN_PLAIN_TOKEN(token::SIZEOF); }

(def) { RETURN_PLAIN_TOKEN(token::DEF); }
(set) { RETURN_PLAIN_TOKEN(token::SET); }
(import) {
    loc->columns(yyleng);
    BEGIN(IMPORT_PATH_STATE);
    return token::IMPORT;
}
(ret) {
    loc->columns(yyleng);
    lval->token = new AstToken(*loc);
    return token::RET;
}
(break) {
    loc->columns(yyleng);
    lval->token = new AstToken(*loc);
    return token::BREAK;
}
(continue) {
    loc->columns(yyleng);
    lval->token = new AstToken(*loc);
    return token::CONTINUE;
}

(if) { RETURN_PLAIN_TOKEN(token::IF); }
(else) { RETURN_PLAIN_TOKEN(token::ELSE); }
(for) { RETURN_PLAIN_TOKEN(token::FOR); }

(struct) { RETURN_PLAIN_TOKEN(token::STRUCT); }
(type|u8|i8|u16|i16|u32|i32|u64|i64|usize|int|uint|f32|f64|bool) {
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::Field, yytext, *loc);
    return token::TYPE;
}
(class) {}
(trait) {}
(case) {}


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

{NUMERIC_LITERAL} {
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::ConstNumeric, yytext, *loc);
    return token::CONST;
}

0b[0-9A-Za-z_]* {
    throwInvalidNumericLiteral(
        loc, yytext, yyleng,
        "Use only `0` and `1` after `0b`, with optional `_` separators, for example `0b1010_1100_u8`.");
}

0o[0-9A-Za-z_]* {
    throwInvalidNumericLiteral(
        loc, yytext, yyleng,
        "Use only digits `0` through `7` after `0o`, with optional `_` separators, for example `0o755_u16`.");
}

0x[0-9A-Za-z_]* {
    throwInvalidNumericLiteral(
        loc, yytext, yyleng,
        "Use hexadecimal digits after `0x`, with optional `_` separators, for example `0x1234_ABCD_u64`.");
}

{DEC_FLOAT}[A-Za-z][A-Za-z0-9_]* {
    throwInvalidNumericLiteral(
        loc, yytext, yyleng,
        "Numeric type suffixes must use `_type`, for example `1.5_f32` or `42_u64`.");
}

{DEC_DIGITS}[A-Za-z][A-Za-z0-9_]* {
    throwInvalidNumericLiteral(
        loc, yytext, yyleng,
        "Numeric type suffixes must use `_type`, for example `1.5_f32` or `42_u64`.");
}

(\"([^\\\"]|\\.)*\") {
    char* strpos = yytext + 1;
    yytext[yyleng - 1] = '\0';
    string escaped = strEscape(string(strpos));
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::ConstStr, escaped, *loc);
    return token::CONST;
}

(\'([^\\\']|\\.)*\') {
    char* strpos = yytext + 1;
    yytext[yyleng - 1] = '\0';
    string escaped = strEscape(string(strpos));
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::ConstChar, escaped, *loc);
    return token::CONST;
}

[a-zA-Z_][a-zA-Z0-9_]* {
    loc->columns(yyleng);
    lval->token = new AstToken(TokenType::Field, yytext, *loc);
    return token::FIELD;
}

(&<) {
    RETURN_PLAIN_TOKEN(token::FUNC_PTR_OPEN);
}

(\+=) {
    RETURN_PLAIN_TOKEN(token::ASSIGN_ADD);
}

(\*=) {
    RETURN_PLAIN_TOKEN(token::ASSIGN_MUL);
}

(\/=) {
    RETURN_PLAIN_TOKEN(token::ASSIGN_DIV);
}

(%=) {
    RETURN_PLAIN_TOKEN(token::ASSIGN_MOD);
}

(-=) {
    RETURN_PLAIN_TOKEN(token::ASSIGN_SUB);
}

(<<=) {
    RETURN_PLAIN_TOKEN(token::ASSIGN_SHL);
}

(>>=) {
    RETURN_PLAIN_TOKEN(token::ASSIGN_SHR);
}

(&=) {
    RETURN_PLAIN_TOKEN(token::ASSIGN_AND);
}

(\^=) {
    RETURN_PLAIN_TOKEN(token::ASSIGN_XOR);
}

(\|=) {
    RETURN_PLAIN_TOKEN(token::ASSIGN_OR);
}

(==) {
    RETURN_PLAIN_TOKEN(token::LOGIC_EQUAL);
}

(!=) {
    RETURN_PLAIN_TOKEN(token::LOGIC_NOT_EQUAL);
}

(&&) {
    RETURN_PLAIN_TOKEN(token::LOGIC_AND);
}

(\<=) {
    RETURN_PLAIN_TOKEN(token::LOGIC_LE);
}

(\>=) {
    RETURN_PLAIN_TOKEN(token::LOGIC_GE);
}

(\<\<) {
    RETURN_PLAIN_TOKEN(token::SHIFT_LEFT);
}

(\>\>) {
    RETURN_PLAIN_TOKEN(token::SHIFT_RIGHT);
}

(\|\|) {
    RETURN_PLAIN_TOKEN(token::LOGIC_OR);
}

(\+|-|\*|\/|%|!|~|<|>|\||&|^) {
    RETURN_PLAIN_TOKEN(yytext[0]);
}

(\{|\}) {
    RETURN_PLAIN_TOKEN(yytext[0]);
}

(:|=|@|#) {
    RETURN_PLAIN_TOKEN(yytext[0]);
}

(\() {
    RETURN_PLAIN_TOKEN('(');
}

(\)) {
    RETURN_PLAIN_TOKEN(')');
}

(\[) {
    RETURN_PLAIN_TOKEN('[');
}

(\]) {
    RETURN_PLAIN_TOKEN(']');
}

(,) {
    RETURN_PLAIN_TOKEN(',');
}

(\.) {
    RETURN_PLAIN_TOKEN('.');
}

([ \t]*(\/\/[^\n]*)?\n)+[ \t]*/(else) {
    advanceNewlineSpan(loc, yytext, yyleng);
}

([ ]*\n[ \n]*) {
    advanceNewlineSpan(loc, yytext, yyleng);
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
