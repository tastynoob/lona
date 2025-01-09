single_type
    : FIELD { $$ = new TypeHelper($1.getText()); }
    | '[' type_name ';' type_name_seq ']' { } // tuple
    ;

pointer_suffix
    : '*' {}
    | pointer_suffix '*' {}
    | '[' ']' { $$ = new std::vector<uint64_t>, $$->push_back(pointerType_autoArray); }
    | '[' expr_seq ']' {}
    | '[' ',' expr_seq ']' {}
    ;

func_ptr_head
    : '(' ')' '*' { }
    | '(' ')' '*' pointer_suffix {}
    ;

type_name
    : single_type {  }
    | single_type pointer_suffix { }
    | func_ptr_head {  }
    | func_ptr_head type_name { }
    ;