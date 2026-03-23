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
    ;

postfix_type
    : type_primary { $$ = $1; }
    | postfix_type '*' %prec type_suffix { $$ = new PointerTypeNode($1, 1, @$); }
    | postfix_type '[' '*' ']' %prec type_suffix { $$ = new IndexablePointerTypeNode($1, @$); }
    | postfix_type '[' ']' %prec type_suffix { $$ = new ArrayTypeNode($1, {}, @$); }
    | postfix_type '[' expr_seq ']' %prec type_suffix {
        $$ = new ArrayTypeNode($1, *$3, @$);
        delete $3;
    }
    | postfix_type '[' ',' expr_seq ']' %prec type_suffix {
        auto dims = *$4;
        dims.insert(dims.begin(), (AstNode*)nullptr);
        $$ = new ArrayTypeNode($1, dims, @$);
        delete $4;
    }
    | postfix_type TYPE_CONST %prec type_suffix { $$ = new ConstTypeNode($1, @$); }
    ;

type_name
    : postfix_type { $$ = $1; }
    | func_ptr_type { $$ = $1; }
    ;

func_ptr_head
    : '(' ')' '*' { $$ = new FuncPtrTypeNode({}, nullptr, @$); }
    | '(' func_param_type_seq ')' '*' { $$ = new FuncPtrTypeNode(*$2, nullptr, @$); delete $2; }
    ;

func_ptr_storage_type
    : func_ptr_head { $$ = $1; }
    | func_ptr_storage_type '*' %prec type_suffix { $$ = new PointerTypeNode($1, 1, @$); }
    | func_ptr_storage_type '[' '*' ']' %prec type_suffix { $$ = new IndexablePointerTypeNode($1, @$); }
    | func_ptr_storage_type '[' ']' %prec type_suffix { $$ = new ArrayTypeNode($1, {}, @$); }
    | func_ptr_storage_type '[' expr_seq ']' %prec type_suffix {
        $$ = new ArrayTypeNode($1, *$3, @$);
        delete $3;
    }
    | func_ptr_storage_type '[' ',' expr_seq ']' %prec type_suffix {
        auto dims = *$4;
        dims.insert(dims.begin(), (AstNode*)nullptr);
        $$ = new ArrayTypeNode($1, dims, @$);
        delete $4;
    }
    | func_ptr_storage_type TYPE_CONST %prec type_suffix { $$ = new ConstTypeNode($1, @$); }
    ;

func_ptr_type
    : func_ptr_storage_type { $$ = $1; }
    | func_ptr_storage_type type_name {
        $$ = $1;
        auto *func = findFuncPtrTypeNode($1);
        assert(func != nullptr);
        func->ret = $2;
    }
    ;
