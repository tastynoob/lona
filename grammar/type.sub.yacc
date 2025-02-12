single_type
    : FIELD { $$ = new TypeHelper($1->text); }
    | type_selector { }
    ;

ptr_suffix
    : '*' { $$ = new std::vector<std::pair<int, AstNode*>>; $$->emplace_back(std::make_pair(pointerType_pointer, nullptr)); }
    | '[' ']' { $$ = new std::vector<std::pair<int, AstNode*>>; $$->emplace_back(std::make_pair(pointerType_autoArray, nullptr)); }
    | '[' expr_seq ']' {
        $$ = new std::vector<std::pair<int, AstNode*>>;
        for (auto it = $2->rbegin(); it != $2->rend(); ++it) {
            $$->emplace_back(std::make_pair(pointerType_fixedArray, *it));
        }
        delete $2;
    }
    | '[' ',' expr_seq ']' {
        $$ = new std::vector<std::pair<int, AstNode*>>;
        for (auto it = $3->rbegin(); it != $3->rend(); ++it) {
            $$->emplace_back(std::make_pair(pointerType_fixedArray, *it));
        }
        $$->emplace_back(std::make_pair(pointerType_autoArray, nullptr));
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
    : '!' '(' ')' { $$ = new TypeHelper("!()"); }
    | '!' '(' type_name_seq ')' { $$ = new TypeHelper("!()"); $$->func_args = $3; }
    | '!' '(' ')' ptr_suffix { $$ = new TypeHelper("!()"); $$->levels = $4; }
    | '!' '(' type_name_seq ')' ptr_suffix { $$ = new TypeHelper("!()"); $$->func_args = $3; $$->levels = $5; }
    ;

type_name
    : single_type { $$ = $1; }
    | single_type ptr_suffix { $$ = $1; $$->levels = $2; }
    | func_ptr_head { $$ = $1; }
    | func_ptr_head type_name { $$ = $1; $1->func_retType = $2; }
    ;