
expr_seq
    : expr { $$ = new std::vector<AstNode*>; $$->emplace_back($1); }
    | expr_seq ',' expr { $$ = $1; $$->emplace_back($3); }
    ;

var_decl_seq
    : var_decl { $$ = new std::vector<AstNode*>; $$->emplace_back($1); }
    | var_decl_seq ',' var_decl { $$ = $1; $$->emplace_back($3); }
    ;

type_name_seq
    : type_name { $$ = new std::vector<TypeNode*>; $$->emplace_back($1); }
    | type_name_seq ',' type_name { $$ = $1; $$->emplace_back($3); }
    ;