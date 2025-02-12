%require "3.8"
%language "c++"

%code requires
{
    namespace lona {
        class Driver;
        class AstToken;
        class AstNode;
        class TypeHelper;
    }

    #include <cstdint>
}

%code
{
    #include "scan/driver.hh"
    #include "ast/token.hh"
    #include "ast/astnode.hh"

    #undef yylex
    #define yylex driver.token

    #define YY_NULLPTR nullptr
}

%union {
    AstToken* token;
    AstNode* node;
    TypeHelper* type_helper;
    std::vector<AstNode*>* seq;
    std::vector<TypeHelper*>* type_seq;
    std::vector<std::pair<int, AstNode*>>* pointer_suffix;
}

%locations
%define api.namespace { lona }
%define api.parser.class { Parser }
%parse-param { Driver &driver }

%token <token> CONST FIELD
%token LOGIC_EQUAL LOGIC_AND LOGIC_OR
%token TRUE FALSE
%token IF ELSE FOR
%token DEF RET STRUCT
%token NEWLINE

%nonassoc type_suffix
%nonassoc LOWER_THAN_ELSE
%nonassoc ELSE
%right '='
%left '&' '|'
%left '+' '-' // + -
%left '*' '/' // * /
%left '.'
%left '(' ')' '[' ']'
%right unary

%type <node> pragram
%type <node> struct_decl func_decl
%type <node> stat_list stat stat_compound stat_if stat_ret stat_expr
%type <node> field_call
%type <node> variable final_expr expr_assign_left expr_getpointee expr expr_assign expr_binOp expr_unary 
%type <node> expr_paren single_value field_selector type_selector
%type <node> var_decl

%type <type_helper> single_type func_ptr_head type_name
%type <pointer_suffix> ptr_suffix

%type <seq> expr_seq var_decl_seq
%type <type_seq> type_name_seq

%start pragram

%%

pragram
    : stat_list { driver.tree = $$ = new AstProgram($1); }
    ;

stat_list
    : NEWLINE { $$ = new AstStatList(); }
    | stat { $$ = new AstStatList($1); }
    | stat_list NEWLINE { $$ = $1;}
    | stat_list stat { $$ = $1; ($1)->as<AstStatList>().push($2);}
    ;

stat
    : stat_expr { $$ = $1; }
    | struct_decl { $$ = $1; }
    | func_decl { $$ = $1; }
    | stat_ret { $$ = $1; }
    | stat_compound { $$ = $1; }
    | stat_if { $$ = $1; }
    ;

stat_compound
    : '{' stat_list '}' { $$ = $2; }
    | '{' '}' { $$ = new AstStatList(); }
    ;

stat_if
    : IF expr_paren stat %prec LOWER_THAN_ELSE { $$ = new AstIf($2, $3); }
    | IF expr_paren stat ELSE stat { $$ = new AstIf($2, $3, $5); }
    ;

stat_ret
    : RET expr NEWLINE { $$ = new AstRet($2); }
    | RET NEWLINE { $$ = new AstRet(nullptr); }
    ;

func_decl
    : DEF FIELD '(' ')' stat_compound { $$ = new AstFuncDecl(*$2, $5); }
    | DEF FIELD '(' ')' '=' type_name stat_compound { $$ = new AstFuncDecl(*$2, $7, nullptr, $6); }
    | DEF FIELD '(' var_decl_seq ')' stat_compound { $$ = new AstFuncDecl(*$2, $6, $4); }
    | DEF FIELD '(' var_decl_seq ')' type_name '=' stat_compound { $$ = new AstFuncDecl(*$2, $8, $4, $6); }
    ;

struct_decl
    : STRUCT FIELD stat_compound { $$ = new AstStructDecl(*$2, $3); }
    ;

var_decl
    : FIELD type_name { $$ = new AstVarDecl(*$1, $2); }
    | FIELD type_name '=' expr { $$ = new AstVarDecl(*$1, $2, $4); }
    | FIELD ':' '=' expr { $$ = new AstVarDecl(*$1, nullptr, $4); }
    ;

stat_expr
    : final_expr NEWLINE { $$ = $1; }
    | var_decl NEWLINE { $$ = $1; }
    ;

final_expr
    : expr { $$ = $1; }
    | expr_assign { $$ = $1; }
    ;

expr
    : expr_binOp { $$ = $1; }
    | expr_unary { $$ = $1; }
    | single_value { $$ = $1; }
    | error { std::cout<<yylhs.location; exit(-1); }
    ;

expr_assign
    : expr_assign_left '=' expr { $$ = new AstAssign($1, $3); }
    ;

expr_assign_left
    : variable { $$ = $1; }
    | expr_getpointee { $$ = $1; }
    ;

expr_binOp
    : expr '*' expr { $$ = new AstBinOper($1, '*', $3); }
    | expr '/' expr { $$ = new AstBinOper($1, '/', $3); }
    | expr '+' expr { $$ = new AstBinOper($1, '+', $3); }
    | expr '-' expr { $$ = new AstBinOper($1, '-', $3); }
    | expr '&' expr { $$ = new AstBinOper($1, '&', $3); }
    ;

expr_unary
    : '!' single_value %prec unary { $$ = new AstUnaryOper(0, $2); }
    | '~' single_value %prec unary { $$ = new AstUnaryOper(0, $2); }
    | '+' single_value %prec unary { $$ = new AstUnaryOper(0, $2); }
    | '-' single_value %prec unary { $$ = new AstUnaryOper(0, $2); }
    | '&' single_value %prec unary { $$ = new AstUnaryOper(0, $2); }
    | expr_getpointee {}
    ;

expr_getpointee
    : '*' single_value %prec unary { $$ = new AstUnaryOper(0, $2); }
    ;

expr_paren
    : '(' expr ')' { $$ = $2; }
    ;

single_value
    : variable { $$ = $1; }
    | CONST { $$ = new AstConst(*$1); }
    | TRUE {}
    | FALSE {}
    | field_call { $$ = $1; }
    | expr_paren {}
    | '(' expr ',' expr_seq ')' {} // tuple
    ;

field_call
    : single_value '(' ')' { $$ = new AstFieldCall($1); }
    | single_value '(' expr_seq ')' { $$ = new AstFieldCall($1, $3); }
    ;

variable
    : FIELD { $$ = new AstField(*$1); }
    | field_selector { $$ = $1; }
    ;

field_selector
    : single_value '.' FIELD { $$ = new AstSelector($1, $3); }
    ;

type_selector
    : FIELD '.' FIELD {}
    | type_selector '.' FIELD {}
    ;

%include type.sub.yacc
%include seq.sub.yacc
// %include func.sub.yacc


%%

void lona::Parser::error(const location_type &l, const std::string &err_message) {
    std::cerr << "Error at " << l << ": " << err_message << std::endl;
}