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

(true) { loc->columns(yyleng); return token::TRUE; }
(false) { loc->columns(yyleng); return token::FALSE; }
(null) { loc->columns(yyleng); return token::NULL_KW; }
(var) { loc->columns(yyleng); return token::VAR; }
(ref) { loc->columns(yyleng); return token::REF; }
(const) { loc->columns(yyleng); return token::TYPE_CONST; }
(cast) { loc->columns(yyleng); return token::CAST; }
(sizeof) { loc->columns(yyleng); return token::SIZEOF; }

(def) { loc->columns(yyleng); return token::DEF; }
(import) { loc->columns(yyleng); BEGIN(IMPORT_PATH_STATE); return token::IMPORT; }
(ret) { loc->columns(yyleng); lval->token = new AstToken(*loc); return token::RET; }
(break) { loc->columns(yyleng); lval->token = new AstToken(*loc); return token::BREAK; }
(continue) { loc->columns(yyleng); lval->token = new AstToken(*loc); return token::CONTINUE; }

(if) { loc->columns(yyleng); return token::IF; }
(else) { loc->columns(yyleng); return token::ELSE; }
(for) { loc->columns(yyleng); return token::FOR; }

(struct) { loc->columns(yyleng); return token::STRUCT; }
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
    loc->columns(yyleng);
    return token::FUNC_PTR_OPEN;
}

(\+=) {
    loc->columns(yyleng);
    return token::ASSIGN_ADD;
}

(\*=) {
    loc->columns(yyleng);
    return token::ASSIGN_MUL;
}

(\/=) {
    loc->columns(yyleng);
    return token::ASSIGN_DIV;
}

(%=) {
    loc->columns(yyleng);
    return token::ASSIGN_MOD;
}

(-=) {
    loc->columns(yyleng);
    return token::ASSIGN_SUB;
}

(<<=) {
    loc->columns(yyleng);
    return token::ASSIGN_SHL;
}

(>>=) {
    loc->columns(yyleng);
    return token::ASSIGN_SHR;
}

(&=) {
    loc->columns(yyleng);
    return token::ASSIGN_AND;
}

(\^=) {
    loc->columns(yyleng);
    return token::ASSIGN_XOR;
}

(\|=) {
    loc->columns(yyleng);
    return token::ASSIGN_OR;
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

(\<=) {
    loc->columns(yyleng);
    return token::LOGIC_LE;
}

(\>=) {
    loc->columns(yyleng);
    return token::LOGIC_GE;
}

(\<\<) {
    loc->columns(yyleng);
    return token::SHIFT_LEFT;
}

(\>\>) {
    loc->columns(yyleng);
    return token::SHIFT_RIGHT;
}

(\|\|) {
    // ||
    loc->columns(yyleng);
    return token::LOGIC_OR;
}

(\+|-|\*|\/|%|!|~|<|>|\||&|^) {
    // + - * / % ! ~ < > | & ^
    loc->columns(yyleng);
    return yytext[0];
}

(\{|\}) {
    // { }
    loc->columns(yyleng);
    return yytext[0];
}

(:|=|\(|\)|\[|\]|@|#|,|\.) {
    // : = ( ) [ ] @ # , .
    loc->columns(yyleng);
    return yytext[0];
}

([ ]*\n[ \t]*)/(else) {
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
