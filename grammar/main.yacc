%require "3.8"
%language "c++"

%code requires
{
    namespace lona {
        class Driver;
    }

    #include "ast/token.hh"
    #include "ast/astnode.hh"
}


%code
{
    #include "scan/driver.hh"

    #undef yylex
    #define yylex driver.token

    #define YY_NULLPTR nullptr
}

%locations
%define api.namespace {lona}
%define api.parser.class {Parser}
%define api.value.type variant
%parse-param { Driver &driver }

%token <lona::AstToken> CONST FIELD
%token <lona::AstToken> OPERLV0 OPERLV1 OPERLV2 OPERLV3 OPERUNARY
%token TRUE FALSE
%token IF ELSE FOR
%token DEF RET
%token NEWLINE

%nonassoc LOWER_THAN_ELSE
%nonassoc ELSE
%right '='
%left OPERLV3 // && ||
%left OPERLV2 // < > <= >= == !=
%left OPERLV1 // + -
%left '*' OPERLV0 // * /
%right unary
%right '(' ')' '[' ']'

%type <lona::AstNode*> pragram
%type <lona::AstNode*> func_decl
%type <lona::AstNode*> stat_list stat stat_compound stat_if stat_ret stat_expr
%type <lona::AstNode*> var_init_assign field_call
%type <lona::AstNode*> final_expr expr expr_left expr_assign expr_binOp expr_unary 
%type <lona::AstNode*> expr_paren single_value field_call_simple
%type <lona::AstVarDecl*> var_decl

%type <lona::TypeHelper*> single_type func_ptr_head type_name
%type <std::vector<uint64_t>*> pointer_suffix

%type field_seq expr_seq var_decl_seq

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
    : IF '(' expr ')' stat %prec LOWER_THAN_ELSE { $$ = new AstIf($3, $5); }
    | IF '(' expr ')' stat ELSE stat { $$ = new AstIf($3, $5, $7); }
    ;

stat_ret
    : RET expr NEWLINE { $$ = new AstRet($2); }
    | RET NEWLINE { $$ = new AstRet(nullptr); }
    ;

func_decl
    : DEF FIELD '(' ')' stat_compound { $$ = new AstFuncDecl($2, $5); }
    | DEF FIELD '(' ')' ':' FIELD stat_compound { auto __t = new AstFuncDecl($2, $7); __t->setRetType($6); $$ = __t; }
    ;

var_decl
    : FIELD type_name { $$ = new AstVarDecl($1, $2); }
    ;

stat_expr
    : final_expr NEWLINE { $$ = $1; }
    ;

var_init_assign
    : FIELD ':' '=' expr { $$ = new AstVarInitAssign(new AstField($1), $4);  } // auto type inference 
    | var_decl '=' expr { $$ = new AstVarInitAssign($1, $3); }
    ;

final_expr
    : expr {}
    | expr_assign {}
    | var_init_assign {}
    ;

expr
    : expr_paren { $$ = $1; }
    | expr_binOp { $$ = $1; }
    | expr_unary { $$ = $1; }
    | single_value { $$ = $1; }
    | field_call_simple {}
    ;

expr_left
    : FIELD { $$ = new AstField($1); }
    ;

expr_assign
    : expr_left '=' expr { $$ = new AstAssign($1, $3); }
    ;

expr_binOp
    : expr OPERLV0 expr { $$ = new AstBinOper($1, $2, $3); }
    | expr OPERLV1 expr { $$ = new AstBinOper($1, $2, $3); }
    | expr OPERLV2 expr { $$ = new AstBinOper($1, $2, $3); }
    | expr OPERLV3 expr { $$ = new AstBinOper($1, $2, $3); }
    | expr '*' expr {}
    ;

expr_unary
    : OPERUNARY expr_paren %prec unary { $$ = new AstUnaryOper($1, $2); }
    | OPERLV1 expr_paren %prec unary { $$ = new AstUnaryOper($1, $2); }
    | OPERUNARY single_value %prec unary { $$ = new AstUnaryOper($1, $2); }
    | OPERLV1 single_value %prec unary { $$ = new AstUnaryOper($1, $2); }
    | '*' FIELD %prec unary {}
    | '*' expr_paren %prec unary {}
    ;

expr_paren
    : '(' expr ')' { $$ = $2; }
    ;

single_value
    : FIELD { $$ = new AstField($1); }
    | CONST { $$ = new AstConst($1); }
    | field_call { $$ = $1; }
    ;

field_call
    : FIELD '(' ')' {  }
    | FIELD '(' expr_seq ')' { }
    ;

field_call_simple
    : FIELD single_value {}
    ;

%include type.sub.yacc
%include seq.sub.yacc
// %include func.sub.yacc


%%

void lona::Parser::error(const location_type &l, const std::string &err_message) {
    std::cerr << "Error at " << l << ": " << err_message << std::endl;
}