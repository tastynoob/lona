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

type_primary
    : single_type { $$ = $1; }
    | tuple_type { $$ = $1; }
    | func_ptr_type { $$ = $1; }
    ;

type_name
    : type_primary { $$ = $1; }
    | type_name '*' %prec type_suffix { $$ = new PointerTypeNode($1, 1, @$); }
    | type_name '[' '*' ']' %prec type_suffix { $$ = new IndexablePointerTypeNode($1, @$); }
    | type_name '[' ']' %prec type_suffix { $$ = new ArrayTypeNode($1, {}, @$); }
    | type_name '[' expr_seq ']' %prec type_suffix {
        $$ = new ArrayTypeNode($1, *$3, @$);
        delete $3;
    }
    | type_name '[' ',' expr_seq ']' %prec type_suffix {
        auto dims = *$4;
        dims.insert(dims.begin(), (AstNode*)nullptr);
        $$ = new ArrayTypeNode($1, dims, @$);
        delete $4;
    }
    | type_name TYPE_CONST %prec type_suffix { $$ = new ConstTypeNode($1, @$); }
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
