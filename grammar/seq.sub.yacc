
expr_seq
    : expr { $$ = new std::vector<AstNode*>; $$->emplace_back($1); }
    | expr_seq ',' expr { $$ = $1; $$->emplace_back($3); }
    ;

param_decl_seq
    : param_decl { $$ = new std::vector<AstNode*>; $$->emplace_back($1); }
    | param_decl_seq ',' param_decl { $$ = $1; $$->emplace_back($3); }
    ;

type_name_seq
    : type_name { $$ = new std::vector<TypeNode*>; $$->emplace_back($1); }
    | type_name_seq ',' type_name { $$ = $1; $$->emplace_back($3); }
    ;

func_param_type_seq
    : func_param_type { $$ = new std::vector<TypeNode*>; $$->emplace_back($1); }
    | func_param_type_seq ',' func_param_type { $$ = $1; $$->emplace_back($3); }
    ;
