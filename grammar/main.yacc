%require "3.8"
%language "c++"

%code requires
{
    namespace lona {
        class Driver;
        class AstToken;
        class AstTag;
        class AstNode;
        class AstStatList;
        class AstVarDecl;
        class TypeNode;
    }

    #include <cstdint>
    #include <vector>
}

%code
{
    #include "lona/err/err.hh"
    #include "lona/scan/driver.hh"
    #include "lona/ast/token.hh"
    #include "lona/ast/astnode.hh"
    #include <string>

    #undef yylex
    #define yylex driver.token

    #define YY_NULLPTR nullptr
}

%union {
    int64_t counter;

    AstToken* token;
    AstTag* tag;
    AstNode* node;
    AstStatList* stat_list;
    AstVarDecl* var_decl;
    std::vector<AstNode*>* seq;
    std::vector<AstTag*>* tags;
    std::vector<AstToken*>* token_seq;

    TypeNode* typeNode;
    std::vector<TypeNode*>* type_seq;
    std::vector<AstNode*>* pointer_suffix;
}

%locations
%define api.namespace { lona }
%define api.parser.class { Parser }
%define parse.error detailed
%define parse.lac full
%parse-param { Driver &driver }

%token <token> CONST "literal" FIELD "identifier" RET "ret" BREAK "break" CONTINUE "continue" TYPE "builtin type"
%token <token> IMPORT_PATH "import path"
%token LOGIC_EQUAL "==" LOGIC_NOT_EQUAL "!=" LOGIC_AND "&&" LOGIC_OR "||"
%token LOGIC_LE "<=" LOGIC_GE ">=" SHIFT_LEFT "<<" SHIFT_RIGHT ">>"
%token VAR "var"
%token REF "ref"
%token TYPE_CONST "const"
%token CAST "cast"
%token SIZEOF "sizeof"
%token TRUE "true" FALSE "false" NULL_KW "null"
%token IF "if" ELSE "else" FOR "for"
%token IMPORT "import"
%token DEF "def" STRUCT "struct"
%token FUNC_PTR_OPEN "&<"
%token NEWLINE "newline"
%token ASSIGN_ADD "+=" ASSIGN_SUB "-="
%token ASSIGN_MUL "*=" ASSIGN_DIV "/=" ASSIGN_MOD "%="
%token ASSIGN_AND "&=" ASSIGN_XOR "^=" ASSIGN_OR "|="
%token ASSIGN_SHL "<<=" ASSIGN_SHR ">>="

%nonassoc type_suffix
%nonassoc LOWER_THAN_ELSE
%nonassoc ELSE
%right '=' ASSIGN_ADD ASSIGN_SUB ASSIGN_MUL ASSIGN_DIV ASSIGN_MOD
%right ASSIGN_AND ASSIGN_XOR ASSIGN_OR ASSIGN_SHL ASSIGN_SHR
%left LOGIC_OR
%left LOGIC_AND
%left '|'
%left '^'
%left '&'
%left LOGIC_EQUAL LOGIC_NOT_EQUAL
%left '<' '>' LOGIC_LE LOGIC_GE
%left SHIFT_LEFT SHIFT_RIGHT
%left '+' '-' // + -
%left '*' '/' '%' // * /
%left '.'
%left '(' ')' '[' ']'
%right unary

%type <node> pragram pragram_stat
%type <node> struct_decl func_decl import_stat
%type <node> struct_stat stat
%type <node> stat_if stat_for stat_ret stat_break stat_continue stat_expr
%type <node> call_like cast_expr sizeof_expr tuple_literal brace_init brace_init_item call_arg named_call_arg
%type <node> variable final_expr expr_assign_left expr_getpointee expr expr_assign expr_binOp expr_unary
%type <node> expr_paren atom_expr postfix_expr dot_like func_pointer_expr dot_like_name
%type <node> param_decl var_def
%type <stat_list> pragram_statlist struct_statlist stat_list stat_compound
%type <var_decl> var_decl

%type <typeNode> single_type type_primary postfix_type func_ptr_type type_name tuple_type func_param_type

%type <seq> expr_seq param_decl_seq brace_inline_body brace_line_body brace_line_entry_seq call_arg_seq
%type <type_seq> type_name_seq
%type <type_seq> func_param_type_seq
%type <counter> opt_newlines
%type <tag> tag_entry
%type <tags> tag_line tag_entry_seq
%type <token_seq> tag_arg_seq
%type <node> tag_stat

%start pragram

%%

pragram
    : pragram_statlist {
        driver.tree = $$ = new AstProgram($1);
    }
    ;

pragram_statlist
    : NEWLINE {
        $$ = new AstStatList();
    }
    | pragram_stat {
        $$ = new AstStatList($1);
    }
    | pragram_statlist NEWLINE {
        $$ = $1;
    }
    | pragram_statlist pragram_stat {
        $$ = $1;
        $$->push($2);
    }
    ;

pragram_stat
    : stat { $$ = $1; }
    | import_stat { $$ = $1; }
    ;

import_stat
    : IMPORT IMPORT_PATH NEWLINE {
        $$ = new AstImport(@$, *$2);
    }
    ;

stat_list
    : NEWLINE {
        $$ = new AstStatList();
    }
    | stat {
        $$ = new AstStatList($1);
    }
    | stat_list NEWLINE {
        $$ = $1;
    }
    | stat_list stat {
        $$ = $1;
        $$->push($2);
    }
    ;

stat
    : stat_expr {
        $$ = $1;
    }
    | struct_decl {
        $$ = $1;
    }
    | func_decl {
        $$ = $1;
    }
    | tag_stat {
        $$ = $1;
    }
    | stat_ret {
        $$ = $1;
    }
    | stat_break {
        $$ = $1;
    }
    | stat_continue {
        $$ = $1;
    }
    | stat_compound {
        $$ = $1;
    }
    | stat_if {
        $$ = $1;
    }
    | stat_for {
        $$ = $1;
    }
    ;

stat_compound
    : '{' stat_list '}' {
        $$ = $2;
    }
    | '{' '}' {
        $$ = new AstStatList();
    }
    ;

stat_if
    : IF expr stat_compound %prec LOWER_THAN_ELSE {
        $$ = new AstIf($2, $3);
    }
    | IF expr stat_compound ELSE stat_compound {
        $$ = new AstIf($2, $3, $5);
    }
    ;

stat_for
    : FOR expr stat_compound {
        $$ = new AstFor($2, $3);
    }
    | FOR expr stat_compound ELSE stat_compound {
        $$ = new AstFor($2, $3, $5);
    }
    ;

stat_ret
    : RET expr NEWLINE {
        $$ = new AstRet($1->loc, $2);
    }
    | RET NEWLINE {
        $$ = new AstRet($1->loc, nullptr);
    }
    ;

stat_break
    : BREAK NEWLINE {
        $$ = new AstBreak($1->loc);
    }
    ;

stat_continue
    : CONTINUE NEWLINE {
        $$ = new AstContinue($1->loc);
    }
    ;

func_decl
    : DEF FIELD '(' ')' NEWLINE { $$ = new AstFuncDecl(*$2, nullptr); }
    | DEF FIELD '(' ')' type_name NEWLINE { $$ = new AstFuncDecl(*$2, nullptr, nullptr, $5); }
    | DEF FIELD '(' param_decl_seq ')' NEWLINE { $$ = new AstFuncDecl(*$2, nullptr, $4); }
    | DEF FIELD '(' param_decl_seq ')' type_name NEWLINE { $$ = new AstFuncDecl(*$2, nullptr, $4, $6); }
    | DEF FIELD '(' ')' stat_compound { $$ = new AstFuncDecl(*$2, $5); }
    | DEF FIELD '(' ')' type_name stat_compound { $$ = new AstFuncDecl(*$2, $6, nullptr, $5); }
    | DEF FIELD '(' param_decl_seq ')' stat_compound { $$ = new AstFuncDecl(*$2, $6, $4); }
    | DEF FIELD '(' param_decl_seq ')' type_name stat_compound { $$ = new AstFuncDecl(*$2, $7, $4, $6); }
    ;

var_decl
    : FIELD type_name { $$ = new AstVarDecl(BindingKind::Value, *$1, $2); }
    ;

param_decl
    : FIELD type_name { $$ = new AstVarDecl(BindingKind::Value, *$1, $2); }
    | REF FIELD type_name { $$ = new AstVarDecl(BindingKind::Ref, *$2, $3); }
    ;

/* struct decl */
struct_decl
    : STRUCT FIELD NEWLINE { $$ = new AstStructDecl(*$2, nullptr); }
    | STRUCT FIELD struct_statlist '}' {
        $$ = new AstStructDecl(*$2, $3);
    }
    ;

struct_statlist
    : '{' struct_stat {
        $$ = new AstStatList($2);
    }
    | '{' NEWLINE {
        $$ = new AstStatList();
    }
    | struct_statlist NEWLINE {
        $$ = $1;
    }
    | struct_statlist struct_stat {
        $$ = $1;
        $$->push($2);
    }
    ;

struct_stat
    : var_decl NEWLINE { $$ = $1; }
    | func_decl {
        $$ = $1;
    }
    | tag_stat {
        $$ = $1;
    }
    ;

/* var define */
var_def
    : VAR var_decl { $$ = new AstVarDef($2); }
    | VAR var_decl '=' expr { $$ = new AstVarDef($2, $4); }
    | VAR var_decl '=' brace_init { $$ = new AstVarDef($2, $4); }
    | REF FIELD type_name '=' expr {
        $$ = new AstVarDef(new AstVarDecl(BindingKind::Ref, *$2, $3), $5);
    }
    | REF FIELD type_name '=' brace_init {
        $$ = new AstVarDef(new AstVarDecl(BindingKind::Ref, *$2, $3), $5);
    }
    | VAR FIELD '=' expr { $$ = new AstVarDef(*$2, $4); }
    | VAR FIELD '=' brace_init { $$ = new AstVarDef(*$2, $4); }
    | FIELD ':' '=' expr { $$ = new AstVarDef(*$1, $4); }
    | FIELD ':' '=' brace_init { $$ = new AstVarDef(*$1, $4); }
    ;

/* expression */
stat_expr
    : final_expr NEWLINE { $$ = $1; }
    | var_def NEWLINE { $$ = $1; }
    ;

tag_stat
    : tag_line { $$ = new AstTagNode($1); }
    ;

tag_line
    : '#' '[' tag_entry_seq ']' NEWLINE { $$ = $3; }
    ;

tag_entry_seq
    : tag_entry {
        $$ = new std::vector<AstTag *>;
        $$->push_back($1);
    }
    | tag_entry_seq ',' opt_newlines tag_entry {
        $$ = $1;
        $$->push_back($4);
    }
    ;

tag_entry
    : FIELD {
        $$ = new AstTag(*$1);
    }
    | FIELD tag_arg_seq {
        $$ = new AstTag(*$1, $2);
    }
    ;

tag_arg_seq
    : CONST {
        $$ = new std::vector<AstToken *>;
        $$->push_back($1);
    }
    | FIELD {
        $$ = new std::vector<AstToken *>;
        $$->push_back($1);
    }
    | TYPE {
        $$ = new std::vector<AstToken *>;
        $$->push_back($1);
    }
    | tag_arg_seq CONST {
        $$ = $1;
        $$->push_back($2);
    }
    | tag_arg_seq FIELD {
        $$ = $1;
        $$->push_back($2);
    }
    | tag_arg_seq TYPE {
        $$ = $1;
        $$->push_back($2);
    }
    ;

final_expr
    : expr { $$ = $1; }
    | expr_assign { $$ = $1; }
    ;

expr
    : expr_binOp { $$ = $1; }
    | expr_unary { $$ = $1; }
    | postfix_expr { $$ = $1; }
    | error {
        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Syntax, @$,
            "I couldn't parse this expression.",
            "Check for a missing operand, separator, or unmatched delimiter near here.");
    }
    ;

expr_assign
    : expr_assign_left '=' expr { $$ = new AstAssign($1, $3); }
    | expr_assign_left ASSIGN_ADD expr { $$ = new AstAssign($1, new AstBinOper($1, '+', $3)); }
    | expr_assign_left ASSIGN_SUB expr { $$ = new AstAssign($1, new AstBinOper($1, '-', $3)); }
    | expr_assign_left ASSIGN_MUL expr { $$ = new AstAssign($1, new AstBinOper($1, '*', $3)); }
    | expr_assign_left ASSIGN_DIV expr { $$ = new AstAssign($1, new AstBinOper($1, '/', $3)); }
    | expr_assign_left ASSIGN_MOD expr { $$ = new AstAssign($1, new AstBinOper($1, '%', $3)); }
    | expr_assign_left ASSIGN_AND expr { $$ = new AstAssign($1, new AstBinOper($1, '&', $3)); }
    | expr_assign_left ASSIGN_XOR expr { $$ = new AstAssign($1, new AstBinOper($1, '^', $3)); }
    | expr_assign_left ASSIGN_OR expr { $$ = new AstAssign($1, new AstBinOper($1, '|', $3)); }
    | expr_assign_left ASSIGN_SHL expr {
        $$ = new AstAssign($1, new AstBinOper($1, token::SHIFT_LEFT, $3));
    }
    | expr_assign_left ASSIGN_SHR expr {
        $$ = new AstAssign($1, new AstBinOper($1, token::SHIFT_RIGHT, $3));
    }
    ;

expr_assign_left
    : variable { $$ = $1; }
    | dot_like { $$ = $1; }
    | expr_getpointee { $$ = $1; }
    | call_like { $$ = $1; }
    ;

expr_binOp
    : expr '*' expr { $$ = new AstBinOper($1, '*', $3); }
    | expr '/' expr { $$ = new AstBinOper($1, '/', $3); }
    | expr '+' expr { $$ = new AstBinOper($1, '+', $3); }
    | expr '-' expr { $$ = new AstBinOper($1, '-', $3); }
    | expr '%' expr { $$ = new AstBinOper($1, '%', $3); }
    | expr SHIFT_LEFT expr { $$ = new AstBinOper($1, token::SHIFT_LEFT, $3); }
    | expr SHIFT_RIGHT expr { $$ = new AstBinOper($1, token::SHIFT_RIGHT, $3); }
    | expr '<' expr { $$ = new AstBinOper($1, '<', $3); }
    | expr '>' expr { $$ = new AstBinOper($1, '>', $3); }
    | expr LOGIC_LE expr { $$ = new AstBinOper($1, token::LOGIC_LE, $3); }
    | expr LOGIC_GE expr { $$ = new AstBinOper($1, token::LOGIC_GE, $3); }
    | expr '&' expr { $$ = new AstBinOper($1, '&', $3); }
    | expr '^' expr { $$ = new AstBinOper($1, '^', $3); }
    | expr '|' expr { $$ = new AstBinOper($1, '|', $3); }
    | expr LOGIC_EQUAL expr { $$ = new AstBinOper($1, token::LOGIC_EQUAL, $3); }
    | expr LOGIC_NOT_EQUAL expr { $$ = new AstBinOper($1, token::LOGIC_NOT_EQUAL, $3); }
    | expr LOGIC_AND expr { $$ = new AstBinOper($1, token::LOGIC_AND, $3); }
    | expr LOGIC_OR expr { $$ = new AstBinOper($1, token::LOGIC_OR, $3); }
    ;

expr_unary
    : '!' postfix_expr %prec unary { $$ = new AstUnaryOper('!', $2); }
    | '~' postfix_expr %prec unary { $$ = new AstUnaryOper('~', $2); }
    | '+' postfix_expr %prec unary { $$ = new AstUnaryOper('+', $2); }
    | '-' postfix_expr %prec unary { $$ = new AstUnaryOper('-', $2); }
    | '&' postfix_expr %prec unary { $$ = new AstUnaryOper('&', $2); }
    | expr_getpointee { $$ = $1; }
    ;

expr_getpointee
    : '*' postfix_expr %prec unary { $$ = new AstUnaryOper('*', $2); }
    ;

expr_paren
    : '(' expr ')' { $$ = $2; }
    ;

atom_expr
    : variable { $$ = $1; }
    | CONST { $$ = new AstConst(*$1); }
    | TRUE { $$ = new AstConst(*new AstToken(TokenType::ConstBool, "true", @$)); }
    | FALSE { $$ = new AstConst(*new AstToken(TokenType::ConstBool, "false", @$)); }
    | NULL_KW { $$ = new AstConst(*new AstToken(TokenType::ConstNull, "null", @$)); }
    | cast_expr { $$ = $1; }
    | sizeof_expr { $$ = $1; }
    | func_pointer_expr { $$ = $1; }
    | expr_paren { $$ = $1; }
    | tuple_literal { $$ = $1; }
    ;

postfix_expr
    : atom_expr { $$ = $1; }
    | call_like { $$ = $1; }
    | dot_like { $$ = $1; }
    ;

cast_expr
    : CAST '[' type_name ']' '(' expr ')' {
        $$ = new AstCastExpr($3, $6, @$);
    }
    ;

sizeof_expr
    : SIZEOF '(' expr ')' {
        $$ = new AstSizeofExpr(nullptr, $3, @$);
    }
    | SIZEOF '[' type_name ']' '(' ')' {
        $$ = new AstSizeofExpr($3, nullptr, @$);
    }
    ;

tuple_literal
    : '(' expr ',' expr_seq ')' {
        auto *items = $4;
        items->insert(items->begin(), $2);
        $$ = new AstTupleLiteral(@$, items);
    }
    ;

brace_init
    : '{' '}' {
        $$ = new AstBraceInit(@$, new std::vector<AstNode *>);
    }
    | '{' NEWLINE opt_newlines '}' {
        $$ = new AstBraceInit(@$, new std::vector<AstNode *>);
    }
    | '{' brace_inline_body '}' {
        $$ = new AstBraceInit(@$, $2);
    }
    | '{' NEWLINE brace_line_body '}' {
        $$ = new AstBraceInit(@$, $3);
    }
    ;

brace_inline_body
    : brace_init_item {
        $$ = new std::vector<AstNode *>;
        $$->emplace_back($1);
    }
    | brace_inline_body ',' opt_newlines brace_init_item {
        $$ = $1;
        $$->emplace_back($4);
    }
    ;

brace_init_item
    : expr {
        $$ = new AstBraceInitItem($1);
    }
    | brace_init {
        $$ = new AstBraceInitItem($1);
    }
    ;

call_arg
    : expr { $$ = $1; }
    | brace_init { $$ = $1; }
    | REF expr { $$ = new AstRefExpr(@$, $2); }
    | named_call_arg { $$ = $1; }
    ;

named_call_arg
    : FIELD '=' expr {
        $$ = new AstNamedCallArg(*$1, $3);
    }
    | REF FIELD '=' expr {
        $$ = new AstNamedCallArg(*$2, new AstRefExpr(@$, $4));
    }
    | FIELD '=' brace_init {
        $$ = new AstNamedCallArg(*$1, $3);
    }
    ;

call_arg_seq
    : call_arg {
        $$ = new std::vector<AstNode *>;
        $$->emplace_back($1);
    }
    | call_arg_seq ',' opt_newlines call_arg {
        $$ = $1;
        $$->emplace_back($4);
    }
    ;

brace_line_body
    : opt_newlines brace_line_entry_seq {
        $$ = $2;
    }
    ;

brace_line_entry_seq
    : brace_init_item NEWLINE opt_newlines {
        $$ = new std::vector<AstNode *>;
        $$->emplace_back($1);
    }
    | brace_line_entry_seq brace_init_item NEWLINE opt_newlines {
        $$ = $1;
        $$->emplace_back($2);
    }
    ;

opt_newlines
    : /* empty */ { $$ = 0; }
    | opt_newlines NEWLINE { $$ = 0; }
    ;

func_pointer_expr
    : FIELD FUNC_PTR_OPEN '>' { $$ = new AstFuncRef(*$1, new std::vector<TypeNode*>); }
    | FIELD FUNC_PTR_OPEN func_param_type_seq '>' { $$ = new AstFuncRef(*$1, $3); }
    ;

call_like
    : postfix_expr '(' ')' { $$ = new AstFieldCall($1); }
    | postfix_expr '(' call_arg_seq ')' { $$ = new AstFieldCall($1, $3); }
    ;

variable
    : FIELD { $$ = new AstField(*$1); }
    ;

dot_like
    : postfix_expr '.' FIELD { $$ = new AstDotLike($1, $3); }
    ;

dot_like_name
    : FIELD { $$ = new AstField(*$1); }
    | dot_like_name '.' FIELD { $$ = new AstDotLike($1, $3); }
    ;

%include type.sub.yacc
%include seq.sub.yacc
// %include func.sub.yacc


%%

void lona::Parser::error(const location_type &l, const std::string &err_message) {
    throw lona::DiagnosticError(
        lona::DiagnosticError::Category::Syntax, l,
        lona::friendlySyntaxMessage(err_message),
        "Check for a missing separator, unmatched delimiter, or mistyped keyword near here.");
}
