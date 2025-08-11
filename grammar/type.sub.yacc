single_type
    : FIELD { $$ = new NormTypeNode($1->text); }
    | type_selector { }
    ;

ptr_suffix
    : '*' { $$ = new std::vector<AstNode*>; $$->emplace_back((AstNode*)0); }
    | '[' ']' { $$ = new std::vector<AstNode*>; $$->emplace_back((AstNode*)1); }
    | '[' expr_seq ']' {
        $$ = new std::vector<AstNode*>;
        for (auto it = $2->rbegin(); it != $2->rend(); ++it) {
            $$->emplace_back(*it);
        }
        delete $2;
    }
    | '[' ',' expr_seq ']' {
        $$ = new std::vector<AstNode*>;
        for (auto it = $3->rbegin(); it != $3->rend(); ++it) {
            $$->emplace_back(*it);
        }
        $$->emplace_back((AstNode*)1);
        delete $3;
    }
    | ptr_suffix ptr_suffix %prec type_suffix {
        $$ = $1;
        for (auto it : *$2) {
            $$->emplace_back(it);
        }
        delete $2;
    }
    ;

func_ptr_head
    : '*' '(' ')' { $$ = new FuncTypeNode(); }
    | '*' '(' type_name_seq ')' { $$ = new FuncTypeNode(*$3); }
    | '*' '(' ')' ptr_suffix { $$ = createPointerOrArrayTypeNode(new FuncTypeNode(), $4); }
    | '*' '(' type_name_seq ')' ptr_suffix { $$ = createPointerOrArrayTypeNode(new FuncTypeNode(*$3), $5); }
    ;

type_name
    : single_type { $$ = $1; }
    | single_type ptr_suffix { $$ = $$ = createPointerOrArrayTypeNode($1, $2); }
    | func_ptr_head { $$ = $1; }
    | func_ptr_head type_name { $$ = $1; $$->as<FuncTypeNode>()->setRet($2); }
    ;