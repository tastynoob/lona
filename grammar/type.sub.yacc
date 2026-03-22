single_type
    : FIELD { $$ = new BaseTypeNode($1->text, @$); }
    | TYPE { $$ = new BaseTypeNode($1->text, @$); }
    | type_selector { $$ = $1; }
    ;

tuple_type
    : '<' type_name_seq '>' {
        $$ = new TupleTypeNode(*$2, @$);
        delete $2;
    }
    ;

func_param_type
    : type_name { $$ = $1; }
    | REF type_name { $$ = new FuncParamTypeNode(BindingKind::Ref, $2, @$); }
    ;

ptr_type
    : base_type '*' { $$ = new PointerTypeNode($1, 1, @$); }
    ;

indexable_ptr_type
    : base_type '[' '*' ']' { $$ = new IndexablePointerTypeNode($1, @$); }
    ;

array_type
    : base_type '[' ']' { $$ = new ArrayTypeNode($1, {}, @$); }
    | base_type '[' expr_seq ']' {
        $$ = new ArrayTypeNode($1, *$3, @$);
        delete $3;
    }
    | base_type '[' ',' expr_seq ']' {
        auto dims = *$4;
        dims.insert(dims.begin(), (AstNode*)nullptr);
        $$ = new ArrayTypeNode($1, dims, @$);
        delete $4;
    }
    ;

base_type
    : single_type { $$ = $1; }
    | tuple_type { $$ = $1; }
    | ptr_type { $$ = $1; }
    | indexable_ptr_type { $$ = $1; }
    | array_type { $$ = $1; }
    ;

bare_func_head
    : '(' ')' { $$ = new FuncPtrTypeNode({}, nullptr, @$); }
    | '(' func_param_type_seq ')' { $$ = new FuncPtrTypeNode(*$2, nullptr, @$); delete $2; }
    ;

func_ptr_head
    : '(' ')' '*' { $$ = new FuncPtrTypeNode({}, nullptr, @$); }
    | '(' func_param_type_seq ')' '*' { $$ = new FuncPtrTypeNode(*$2, nullptr, @$); delete $2; }
    ;

func_ptr_type
    : func_ptr_head { $$ = $1; }
    | func_ptr_head type_name {
        $$ = $1;
        auto *func = dynamic_cast<FuncPtrTypeNode *>($1);
        assert(func != nullptr);
        func->ret = $2;
    }
    ;

type_name
    : base_type { $$ = $1; }
    | func_ptr_type { $$ = $1; }
    | bare_func_head type_name {
        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Syntax, @$,
            "bare function signatures are not allowed in type positions.",
            "Use an explicit function pointer type like `()*` or `(T1, T2)* Ret` instead.");
    }
    | bare_func_head '[' ']' type_name {
        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Syntax, @$,
            "bare function signatures are not allowed in type positions.",
            "Use an explicit function pointer type like `()*` or `(T1, T2)* Ret` instead.");
    }
    | bare_func_head '[' expr_seq ']' type_name {
        delete $3;
        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Syntax, @$,
            "bare function signatures are not allowed in type positions.",
            "Use an explicit function pointer type like `()*` or `(T1, T2)* Ret` instead.");
    }
    | bare_func_head '[' '*' ']' type_name {
        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Syntax, @$,
            "bare function signatures are not allowed in type positions.",
            "Use an explicit function pointer type like `()*` or `(T1, T2)* Ret` instead.");
    }
    | bare_func_head '[' ',' expr_seq ']' type_name {
        delete $4;
        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Syntax, @$,
            "bare function signatures are not allowed in type positions.",
            "Use an explicit function pointer type like `()*` or `(T1, T2)* Ret` instead.");
    }
    ;
