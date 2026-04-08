%require "3.8"
%language "c++"

%code requires
{
    namespace lona {
        class Driver;
        class AstToken;
        class AstTag;
        class AstGenericParam;
        class AstNode;
        class AstStatList;
        class AstVarDecl;
        class AstGlobalDecl;
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
    #include "lona/ast/type_node_tools.hh"
    #include <string>

    #undef yylex
    #define yylex driver.token

    #define YY_NULLPTR nullptr
}

%union {
    int64_t counter;

    AstToken* token;
    AstTag* tag;
    AstGenericParam* generic_param;
    AstNode* node;
    AstStatList* stat_list;
    AstVarDecl* var_decl;
    std::vector<AstNode*>* seq;
    std::vector<AstTag*>* tags;
    std::vector<AstToken*>* token_seq;
    std::vector<AstGenericParam*>* generic_param_seq;

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
%token GLOBAL "global"
%token REF "ref"
%token TYPE_CONST "const"
%token CAST "cast"
%token SIZEOF "sizeof"
%token TRUE "true" FALSE "false" NULL_KW "null"
%token IF "if" ELSE "else" FOR "for"
%token IMPORT "import"
%token DEF "def" SET "set" STRUCT "struct" TRAIT "trait" IMPL "impl" DYN "dyn"
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
%left FUNC_REF_BIND
%left '.'
%left '(' ')' '[' ']'
%right unary

%type <node> pragram pragram_stat
%type <node> struct_decl trait_decl impl_decl func_decl trait_func_decl import_stat global_decl
%type <node> struct_stat trait_stat stat
%type <node> stat_if stat_for stat_ret stat_break stat_continue stat_expr
%type <node> call_like cast_expr sizeof_expr tuple_literal brace_init brace_init_item call_arg named_call_arg
%type <node> variable final_expr expr_assign_left expr_getpointee expr expr_assign expr_binOp expr_unary
%type <node> expr_paren atom_expr postfix_expr type_apply_expr dot_like dot_like_name func_ref_expr func_ref_target
%type <node> param_decl var_def trait_var_def
%type <stat_list> pragram_statlist struct_statlist trait_statlist stat_list stat_compound
%type <var_decl> field_decl var_decl

%type <typeNode> single_type type_primary postfix_type func_ptr_type type_name tuple_type func_param_type

%type <seq> expr_seq param_decl_seq brace_inline_body brace_line_body brace_line_entry_seq call_arg_seq type_bracket_item_seq
%type <type_seq> type_name_seq
%type <type_seq> func_param_type_seq
%type <counter> opt_newlines opt_brace_line_comma opt_set_prefix
%type <tag> tag_entry
%type <tags> tag_line tag_entry_seq
%type <token_seq> tag_arg_seq
%type <generic_param> generic_param
%type <generic_param_seq> generic_param_seq opt_type_params
%type <node> tag_stat type_bracket_item
%type <typeNode> impl_self_type impl_self_type_atom

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
    | global_decl { $$ = $1; }
    | trait_decl { $$ = $1; }
    | impl_decl { $$ = $1; }
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
    | IF expr stat_compound ELSE stat_if {
        $$ = new AstIf($2, $3, new AstStatList($5));
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

opt_set_prefix
    : %empty { $$ = 0; }
    | SET { $$ = 1; }
    ;

opt_type_params
    : %empty { $$ = nullptr; }
    | '[' opt_newlines generic_param_seq opt_newlines ']' { $$ = $3; }
    ;

generic_param_seq
    : generic_param {
        $$ = new std::vector<AstGenericParam *>;
        $$->push_back($1);
    }
    | generic_param_seq ',' opt_newlines generic_param {
        $$ = $1;
        $$->push_back($4);
    }
    ;

generic_param
    : FIELD {
        $$ = new AstGenericParam(*$1);
    }
    | FIELD dot_like_name {
        $$ = new AstGenericParam(*$1, $2);
    }
    | FIELD dot_like_name '+' opt_newlines dot_like_name {
        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Syntax, @$,
            "generic v0 only supports a single trait bound per type parameter",
            "Write one bound like `[T Hash]` for now. Multi-bound forms like `[T Hash + Eq]` are not supported yet.");
    }
    ;

func_decl
    : opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines ')' NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, $4, nullptr, nullptr, AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines ')' type_name NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, $4, nullptr, $8, AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines param_decl_seq opt_newlines ')' NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, $4, $7, nullptr, AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines param_decl_seq opt_newlines ')' type_name NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, $4, $7, $10, AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines ')' stat_compound {
        $$ = new AstFuncDecl(*$3, $8, $4, nullptr, nullptr, AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines ')' type_name stat_compound {
        $$ = new AstFuncDecl(*$3, $9, $4, nullptr, $8, AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines param_decl_seq opt_newlines ')' stat_compound {
        $$ = new AstFuncDecl(*$3, $10, $4, $7, nullptr, AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines param_decl_seq opt_newlines ')' type_name stat_compound {
        $$ = new AstFuncDecl(*$3, $11, $4, $7, $10, AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    ;

trait_func_decl
    : opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines ')' NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, $4, nullptr, nullptr,
                             AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines ')' type_name NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, $4, nullptr, $8,
                             AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines param_decl_seq opt_newlines ')' NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, $4, $7, nullptr,
                             AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines param_decl_seq opt_newlines ')' type_name NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, $4, $7, $10,
                             AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines ')' stat_compound {
        $$ = new AstFuncDecl(*$3, $8, $4, nullptr, nullptr,
                             AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines ')' type_name stat_compound {
        $$ = new AstFuncDecl(*$3, $9, $4, nullptr, $8,
                             AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines param_decl_seq opt_newlines ')' stat_compound {
        $$ = new AstFuncDecl(*$3, $10, $4, $7, nullptr,
                             AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    | opt_set_prefix DEF FIELD opt_type_params '(' opt_newlines param_decl_seq opt_newlines ')' type_name stat_compound {
        $$ = new AstFuncDecl(*$3, $11, $4, $7, $10,
                             AbiKind::Native,
                             $1 ? AccessKind::GetSet : AccessKind::GetOnly);
    }
    ;

field_decl
    : FIELD type_name {
        $$ = new AstVarDecl(BindingKind::Value, *$1, $2, nullptr,
                            AccessKind::GetOnly, $1->text == string("_"));
    }
    | SET FIELD type_name {
        $$ = new AstVarDecl(BindingKind::Value, *$2, $3, nullptr,
                            AccessKind::GetSet, $2->text == string("_"));
    }
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
    : STRUCT FIELD opt_type_params NEWLINE { $$ = new AstStructDecl(*$2, nullptr, $3); }
    | STRUCT FIELD opt_type_params '{' '}' {
        $$ = new AstStructDecl(*$2, new AstStatList(), $3);
    }
    | STRUCT FIELD opt_type_params struct_statlist '}' {
        $$ = new AstStructDecl(*$2, $4, $3);
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
    : field_decl NEWLINE { $$ = $1; }
    | func_decl {
        $$ = $1;
    }
    | tag_stat {
        $$ = $1;
    }
    ;

trait_decl
    : TRAIT FIELD NEWLINE { $$ = new AstTraitDecl(*$2, nullptr); }
    | TRAIT FIELD '{' '}' {
        $$ = new AstTraitDecl(*$2, new AstStatList());
    }
    | TRAIT FIELD trait_statlist '}' {
        $$ = new AstTraitDecl(*$2, $3);
    }
    ;

trait_statlist
    : '{' trait_stat {
        $$ = new AstStatList($2);
    }
    | '{' NEWLINE {
        $$ = new AstStatList();
    }
    | trait_statlist NEWLINE {
        $$ = $1;
    }
    | trait_statlist trait_stat {
        $$ = $1;
        $$->push($2);
    }
    ;

trait_stat
    : field_decl NEWLINE { $$ = $1; }
    | trait_func_decl {
        $$ = $1;
    }
    | struct_decl {
        $$ = $1;
    }
    | global_decl {
        $$ = $1;
    }
    | trait_var_def NEWLINE {
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
    | tag_stat {
        $$ = $1;
    }
    ;

impl_decl
    : IMPL opt_type_params dot_like_name FOR opt_newlines impl_self_type stat_compound {
        $$ = new AstTraitImplDecl($6, $3, $7, $2, @$);
    }
    ;

/* var define */
var_def
    : VAR var_decl { $$ = new AstVarDef($2); }
    | VAR var_decl '=' opt_newlines expr { $$ = new AstVarDef($2, $5); }
    | VAR var_decl '=' opt_newlines brace_init { $$ = new AstVarDef($2, $5); }
    | TYPE_CONST FIELD '=' opt_newlines expr { $$ = new AstVarDef(*$2, $5, true); }
    | TYPE_CONST FIELD '=' opt_newlines brace_init { $$ = new AstVarDef(*$2, $5, true); }
    | TYPE_CONST var_decl '=' opt_newlines expr { $$ = new AstVarDef($2, $5, true); }
    | TYPE_CONST var_decl '=' opt_newlines brace_init { $$ = new AstVarDef($2, $5, true); }
    | REF FIELD type_name '=' opt_newlines expr {
        $$ = new AstVarDef(new AstVarDecl(BindingKind::Ref, *$2, $3), $6);
    }
    | REF FIELD type_name '=' opt_newlines brace_init {
        $$ = new AstVarDef(new AstVarDecl(BindingKind::Ref, *$2, $3), $6);
    }
    | VAR FIELD '=' opt_newlines expr { $$ = new AstVarDef(*$2, $5); }
    | VAR FIELD '=' opt_newlines brace_init { $$ = new AstVarDef(*$2, $5); }
    | FIELD ':' '=' opt_newlines expr { $$ = new AstVarDef(*$1, $5); }
    | FIELD ':' '=' opt_newlines brace_init { $$ = new AstVarDef(*$1, $5); }
    ;

trait_var_def
    : VAR var_decl { $$ = new AstVarDef($2); }
    | VAR var_decl '=' opt_newlines expr { $$ = new AstVarDef($2, $5); }
    | VAR var_decl '=' opt_newlines brace_init { $$ = new AstVarDef($2, $5); }
    | TYPE_CONST FIELD '=' opt_newlines expr { $$ = new AstVarDef(*$2, $5, true); }
    | TYPE_CONST FIELD '=' opt_newlines brace_init { $$ = new AstVarDef(*$2, $5, true); }
    | TYPE_CONST var_decl '=' opt_newlines expr { $$ = new AstVarDef($2, $5, true); }
    | TYPE_CONST var_decl '=' opt_newlines brace_init { $$ = new AstVarDef($2, $5, true); }
    | REF FIELD type_name '=' opt_newlines expr {
        $$ = new AstVarDef(new AstVarDecl(BindingKind::Ref, *$2, $3), $6);
    }
    | REF FIELD type_name '=' opt_newlines brace_init {
        $$ = new AstVarDef(new AstVarDecl(BindingKind::Ref, *$2, $3), $6);
    }
    | VAR FIELD '=' opt_newlines expr { $$ = new AstVarDef(*$2, $5); }
    | VAR FIELD '=' opt_newlines brace_init { $$ = new AstVarDef(*$2, $5); }
    ;

global_decl
    : GLOBAL FIELD type_name NEWLINE {
        $$ = new AstGlobalDecl(*$2, $3);
    }
    | GLOBAL FIELD type_name '=' opt_newlines expr NEWLINE {
        $$ = new AstGlobalDecl(*$2, $3, $6);
    }
    | GLOBAL FIELD type_name '=' opt_newlines brace_init NEWLINE {
        $$ = new AstGlobalDecl(*$2, $3, $6);
    }
    | GLOBAL FIELD '=' opt_newlines expr NEWLINE {
        $$ = new AstGlobalDecl(*$2, nullptr, $5);
    }
    | GLOBAL FIELD '=' opt_newlines brace_init NEWLINE {
        $$ = new AstGlobalDecl(*$2, nullptr, $5);
    }
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
    : '#' '[' opt_newlines tag_entry_seq opt_newlines ']' NEWLINE { $$ = $4; }
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
    : expr_assign_left '=' opt_newlines expr { $$ = new AstAssign($1, $4); }
    | expr_assign_left ASSIGN_ADD opt_newlines expr {
        $$ = new AstAssign($1, new AstBinOper($1, '+', $4));
    }
    | expr_assign_left ASSIGN_SUB opt_newlines expr {
        $$ = new AstAssign($1, new AstBinOper($1, '-', $4));
    }
    | expr_assign_left ASSIGN_MUL opt_newlines expr {
        $$ = new AstAssign($1, new AstBinOper($1, '*', $4));
    }
    | expr_assign_left ASSIGN_DIV opt_newlines expr {
        $$ = new AstAssign($1, new AstBinOper($1, '/', $4));
    }
    | expr_assign_left ASSIGN_MOD opt_newlines expr {
        $$ = new AstAssign($1, new AstBinOper($1, '%', $4));
    }
    | expr_assign_left ASSIGN_AND opt_newlines expr {
        $$ = new AstAssign($1, new AstBinOper($1, '&', $4));
    }
    | expr_assign_left ASSIGN_XOR opt_newlines expr {
        $$ = new AstAssign($1, new AstBinOper($1, '^', $4));
    }
    | expr_assign_left ASSIGN_OR opt_newlines expr {
        $$ = new AstAssign($1, new AstBinOper($1, '|', $4));
    }
    | expr_assign_left ASSIGN_SHL opt_newlines expr {
        $$ = new AstAssign($1, new AstBinOper($1, token::SHIFT_LEFT, $4));
    }
    | expr_assign_left ASSIGN_SHR opt_newlines expr {
        $$ = new AstAssign($1, new AstBinOper($1, token::SHIFT_RIGHT, $4));
    }
    ;

expr_assign_left
    : variable { $$ = $1; }
    | dot_like { $$ = $1; }
    | expr_getpointee { $$ = $1; }
    | call_like { $$ = $1; }
    ;

expr_binOp
    : expr '*' opt_newlines expr { $$ = new AstBinOper($1, '*', $4); }
    | expr '/' opt_newlines expr { $$ = new AstBinOper($1, '/', $4); }
    | expr '+' opt_newlines expr { $$ = new AstBinOper($1, '+', $4); }
    | expr '-' opt_newlines expr { $$ = new AstBinOper($1, '-', $4); }
    | expr '%' opt_newlines expr { $$ = new AstBinOper($1, '%', $4); }
    | expr SHIFT_LEFT opt_newlines expr {
        $$ = new AstBinOper($1, token::SHIFT_LEFT, $4);
    }
    | expr SHIFT_RIGHT opt_newlines expr {
        $$ = new AstBinOper($1, token::SHIFT_RIGHT, $4);
    }
    | expr '<' opt_newlines expr { $$ = new AstBinOper($1, '<', $4); }
    | expr '>' opt_newlines expr { $$ = new AstBinOper($1, '>', $4); }
    | expr LOGIC_LE opt_newlines expr { $$ = new AstBinOper($1, token::LOGIC_LE, $4); }
    | expr LOGIC_GE opt_newlines expr { $$ = new AstBinOper($1, token::LOGIC_GE, $4); }
    | expr '&' opt_newlines expr { $$ = new AstBinOper($1, '&', $4); }
    | expr '^' opt_newlines expr { $$ = new AstBinOper($1, '^', $4); }
    | expr '|' opt_newlines expr { $$ = new AstBinOper($1, '|', $4); }
    | expr LOGIC_EQUAL opt_newlines expr {
        $$ = new AstBinOper($1, token::LOGIC_EQUAL, $4);
    }
    | expr LOGIC_NOT_EQUAL opt_newlines expr {
        $$ = new AstBinOper($1, token::LOGIC_NOT_EQUAL, $4);
    }
    | expr LOGIC_AND opt_newlines expr { $$ = new AstBinOper($1, token::LOGIC_AND, $4); }
    | expr LOGIC_OR opt_newlines expr { $$ = new AstBinOper($1, token::LOGIC_OR, $4); }
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
    : '(' opt_newlines expr opt_newlines ')' { $$ = $3; }
    ;

atom_expr
    : variable { $$ = $1; }
    | CONST { $$ = new AstConst(*$1); }
    | TRUE { $$ = new AstConst(*new AstToken(TokenType::ConstBool, "true", @$)); }
    | FALSE { $$ = new AstConst(*new AstToken(TokenType::ConstBool, "false", @$)); }
    | NULL_KW { $$ = new AstConst(*new AstToken(TokenType::ConstNull, "null", @$)); }
    | func_ref_expr { $$ = $1; }
    | cast_expr { $$ = $1; }
    | sizeof_expr { $$ = $1; }
    | expr_paren { $$ = $1; }
    | tuple_literal { $$ = $1; }
    ;

func_ref_expr
    : '@' func_ref_target { $$ = new AstFuncRef($2, @$); }
    ;

func_ref_target
    : dot_like_name %prec FUNC_REF_BIND { $$ = $1; }
    | dot_like_name '[' opt_newlines type_name_seq opt_newlines ']' {
        $$ = new AstTypeApply($1, $4, @$);
    }
    ;

postfix_expr
    : atom_expr { $$ = $1; }
    | call_like { $$ = $1; }
    | dot_like { $$ = $1; }
    | type_apply_expr { $$ = $1; }
    ;

cast_expr
    : CAST '[' opt_newlines type_name opt_newlines ']' opt_newlines '(' opt_newlines expr opt_newlines ')' {
        $$ = new AstCastExpr($4, $10, @$);
    }
    ;

sizeof_expr
    : SIZEOF opt_newlines '(' opt_newlines expr opt_newlines ')' {
        $$ = new AstSizeofExpr(nullptr, $5, @$);
    }
    | SIZEOF '[' opt_newlines type_name opt_newlines ']' opt_newlines '(' opt_newlines ')' {
        $$ = new AstSizeofExpr($4, nullptr, @$);
    }
    ;

tuple_literal
    : '(' opt_newlines expr opt_newlines ',' opt_newlines expr_seq opt_newlines ')' {
        auto *items = $7;
        items->insert(items->begin(), $3);
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
    | '{' brace_inline_body ',' opt_newlines '}' {
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
    : FIELD '=' opt_newlines expr {
        $$ = new AstNamedCallArg(*$1, $4);
    }
    | REF FIELD '=' opt_newlines expr {
        $$ = new AstNamedCallArg(*$2, new AstRefExpr(@$, $5));
    }
    | FIELD '=' opt_newlines brace_init {
        $$ = new AstNamedCallArg(*$1, $4);
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
    : brace_init_item opt_brace_line_comma NEWLINE opt_newlines {
        $$ = new std::vector<AstNode *>;
        $$->emplace_back($1);
    }
    | brace_line_entry_seq brace_init_item opt_brace_line_comma NEWLINE opt_newlines {
        $$ = $1;
        $$->emplace_back($2);
    }
    ;

opt_brace_line_comma
    : %empty { $$ = 0; }
    | ',' { $$ = 0; }
    ;

opt_newlines
    : %empty { $$ = 0; }
    | opt_newlines NEWLINE { $$ = 0; }
    ;

type_apply_expr
    : postfix_expr '[' opt_newlines type_name_seq opt_newlines ']' {
        $$ = new AstTypeApply($1, $4, @$);
    }
    ;

call_like
    : postfix_expr '(' opt_newlines ')' { $$ = new AstFieldCall($1); }
    | postfix_expr '(' opt_newlines call_arg_seq opt_newlines ')' { $$ = new AstFieldCall($1, $4); }
    ;

variable
    : FIELD { $$ = new AstField(*$1); }
    ;

dot_like
    : postfix_expr '.' opt_newlines FIELD { $$ = new AstDotLike($1, $4); }
    ;

dot_like_name
    : FIELD { $$ = new AstField(*$1); }
    | dot_like_name '.' opt_newlines FIELD { $$ = new AstDotLike($1, $4); }
    ;

impl_self_type_atom
    : dot_like_name {
        $$ = new BaseTypeNode($1, @$);
    }
    | TYPE {
        $$ = new BaseTypeNode($1->text, @$);
    }
    ;

impl_self_type
    : impl_self_type_atom {
        $$ = $1;
    }
    | impl_self_type '[' opt_newlines type_name_seq opt_newlines ']' %prec type_suffix {
        $$ = new AppliedTypeNode($1, *$4, @$);
        delete $4;
    }
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
