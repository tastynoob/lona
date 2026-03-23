%require "3.8"
%language "c++"

%code requires
{
    namespace lona {
        class Driver;
        class AstToken;
        class AstNode;
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

    static lona::AbiKind
    parseExternAbi(lona::AstToken *token) {
        if (!token || token->type != lona::TokenType::ConstStr) {
            throw lona::DiagnosticError(
                lona::DiagnosticError::Category::Semantic,
                token ? token->loc : lona::location(),
                "extern ABI name must be a string literal",
                "Use syntax like `extern \"C\" def foo(...)`.");
        }

        auto abi = std::string(token->text.tochara(), token->text.size());
        if (abi == "C") {
            return lona::AbiKind::C;
        }

        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Semantic, token->loc,
            "unsupported extern ABI `" + abi + "`",
            "Only `extern \"C\"` is supported right now.");
    }

    static lona::StructDeclKind
    parseStructDeclKind(lona::AstToken *token) {
        if (!token || token->type != lona::TokenType::ConstStr) {
            throw lona::DiagnosticError(
                lona::DiagnosticError::Category::Semantic,
                token ? token->loc : lona::location(),
                "struct repr name must be a string literal",
                "Use syntax like `repr(\"C\") struct Point { ... }`.");
        }

        auto repr = std::string(token->text.tochara(), token->text.size());
        if (repr == "C") {
            return lona::StructDeclKind::ReprC;
        }

        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Semantic, token->loc,
            "unsupported struct repr `" + repr + "`",
            "Only `repr(\"C\")` is supported right now.");
    }
}

%union {
    int64_t counter;

    AstToken* token;
    AstNode* node;
    std::vector<AstNode*>* seq;

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
%token TRUE "true" FALSE "false"
%token IF "if" ELSE "else" FOR "for"
%token IMPORT "import"
%token DEF "def" STRUCT "struct" EXTERN "extern" REPR "repr"
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

%type <node> pragram pragram_statlist pragram_stat
%type <node> struct_decl func_decl import_stat
%type <node> struct_statlist struct_stat stat_list stat
%type <node> stat_compound stat_if stat_for stat_ret stat_break stat_continue stat_expr
%type <node> field_call cast_expr tuple_literal brace_init brace_init_item call_arg named_call_arg
%type <node> variable final_expr expr_assign_left expr_getpointee expr expr_assign expr_binOp expr_unary
%type <node> expr_paren single_value field_selector func_pointer_expr
%type <typeNode> type_selector
%type <node> var_decl param_decl var_def

%type <typeNode> single_type type_primary postfix_type func_ptr_type type_name tuple_type func_param_type

%type <seq> expr_seq param_decl_seq brace_inline_body brace_line_body brace_line_entry_seq call_arg_seq
%type <type_seq> type_name_seq
%type <type_seq> func_param_type_seq
%type <counter> opt_newlines extern_abi struct_decl_kind

%start pragram

%%

pragram
    : pragram_statlist { driver.tree = $$ = new AstProgram($1); }
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
        ($$)->as<AstStatList>()->push($2);
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
        ($$)->as<AstStatList>()->push($2);
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
    : DEF FIELD '(' ')' stat_compound { $$ = new AstFuncDecl(*$2, $5); }
    | DEF FIELD '(' ')' type_name stat_compound { $$ = new AstFuncDecl(*$2, $6, nullptr, $5); }
    | DEF FIELD '(' param_decl_seq ')' stat_compound { $$ = new AstFuncDecl(*$2, $6, $4); }
    | DEF FIELD '(' param_decl_seq ')' type_name stat_compound { $$ = new AstFuncDecl(*$2, $7, $4, $6); }
    | extern_abi DEF FIELD '(' ')' NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, nullptr, nullptr,
                             static_cast<lona::AbiKind>($1));
    }
    | extern_abi DEF FIELD '(' ')' type_name NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, nullptr, $6,
                             static_cast<lona::AbiKind>($1));
    }
    | extern_abi DEF FIELD '(' param_decl_seq ')' NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, $5, nullptr,
                             static_cast<lona::AbiKind>($1));
    }
    | extern_abi DEF FIELD '(' param_decl_seq ')' type_name NEWLINE {
        $$ = new AstFuncDecl(*$3, nullptr, $5, $7,
                             static_cast<lona::AbiKind>($1));
    }
    | extern_abi DEF FIELD '(' ')' stat_compound {
        $$ = new AstFuncDecl(*$3, $6, nullptr, nullptr,
                             static_cast<lona::AbiKind>($1));
    }
    | extern_abi DEF FIELD '(' ')' type_name stat_compound {
        $$ = new AstFuncDecl(*$3, $7, nullptr, $6,
                             static_cast<lona::AbiKind>($1));
    }
    | extern_abi DEF FIELD '(' param_decl_seq ')' stat_compound {
        $$ = new AstFuncDecl(*$3, $7, $5, nullptr,
                             static_cast<lona::AbiKind>($1));
    }
    | extern_abi DEF FIELD '(' param_decl_seq ')' type_name stat_compound {
        $$ = new AstFuncDecl(*$3, $8, $5, $7,
                             static_cast<lona::AbiKind>($1));
    }
    ;

extern_abi
    : EXTERN CONST { $$ = static_cast<int64_t>(parseExternAbi($2)); }
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
    : STRUCT FIELD struct_statlist '}' { $$ = new AstStructDecl(*$2, $3); }
    | EXTERN STRUCT FIELD NEWLINE {
        $$ = new AstStructDecl(*$3, nullptr, lona::StructDeclKind::Extern);
    }
    | EXTERN STRUCT FIELD struct_statlist '}' {
        $$ = new AstStructDecl(*$3, $4, lona::StructDeclKind::Extern);
    }
    | struct_decl_kind STRUCT FIELD struct_statlist '}' {
        $$ = new AstStructDecl(*$3, $4,
                               static_cast<lona::StructDeclKind>($1));
    }
    ;

struct_decl_kind
    : REPR '(' CONST ')' { $$ = static_cast<int64_t>(parseStructDeclKind($3)); }
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
        ($$)->as<AstStatList>()->push($2);
    }
    ;

struct_stat
    : var_decl NEWLINE {
        $$ = $1;
    }
    | func_decl {
        $$ = $1;
    }
    ;

/* var define */
var_def
    : VAR var_decl { $$ = new AstVarDef($2->as<AstVarDecl>()); }
    | VAR var_decl '=' expr { $$ = new AstVarDef($2->as<AstVarDecl>(), $4); }
    | VAR var_decl '=' brace_init { $$ = new AstVarDef($2->as<AstVarDecl>(), $4); }
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

final_expr
    : expr { $$ = $1; }
    | expr_assign { $$ = $1; }
    ;

expr
    : expr_binOp { $$ = $1; }
    | expr_unary { $$ = $1; }
    | single_value { $$ = $1; }
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
    | expr_getpointee { $$ = $1; }
    | field_call { $$ = $1; }
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
    : '!' single_value %prec unary { $$ = new AstUnaryOper('!', $2); }
    | '~' single_value %prec unary { $$ = new AstUnaryOper('~', $2); }
    | '+' single_value %prec unary { $$ = new AstUnaryOper('+', $2); }
    | '-' single_value %prec unary { $$ = new AstUnaryOper('-', $2); }
    | '&' single_value %prec unary { $$ = new AstUnaryOper('&', $2); }
    | expr_getpointee { $$ = $1; }
    ;

expr_getpointee
    : '*' single_value %prec unary { $$ = new AstUnaryOper('*', $2); }
    ;

expr_paren
    : '(' expr ')' { $$ = $2; }
    ;

single_value
    : variable { $$ = $1; }
    | CONST { $$ = new AstConst(*$1); }
    | TRUE { $$ = new AstConst(*new AstToken(TokenType::ConstBool, "true", @$)); }
    | FALSE { $$ = new AstConst(*new AstToken(TokenType::ConstBool, "false", @$)); }
    | cast_expr { $$ = $1; }
    | func_pointer_expr { $$ = $1; }
    | field_call { $$ = $1; }
    | expr_paren { $$ = $1; }
    | tuple_literal { $$ = $1; }
    ;

cast_expr
    : CAST '[' type_name ']' '(' expr ')' {
        $$ = new AstCastExpr($3, $6, @$);
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

field_call
    : single_value '(' ')' { $$ = new AstFieldCall($1); }
    | single_value '(' call_arg_seq ')' { $$ = new AstFieldCall($1, $3); }
    ;

variable
    : FIELD { $$ = new AstField(*$1); }
    | field_selector { $$ = $1; }
    ;

field_selector
    : single_value '.' FIELD { $$ = new AstSelector($1, $3); }
    ;

type_selector
    : FIELD '.' FIELD {
        string name = $1->text;
        name += ".";
        name += $3->text;
        $$ = new BaseTypeNode(name, @$);
    }
    | type_selector '.' FIELD {
        auto *base = dynamic_cast<BaseTypeNode *>($1);
        assert(base);
        string name = base->name;
        name += ".";
        name += $3->text;
        $$ = new BaseTypeNode(name, @$);
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
