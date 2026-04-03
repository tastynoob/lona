type_bracket_item
    : expr { $$ = $1; }
    | TYPE { $$ = new AstField($1->text, $1->loc); }
    ;

type_bracket_item_seq
    : type_bracket_item {
        $$ = new std::vector<AstNode *>;
        $$->push_back($1);
    }
    | type_bracket_item_seq ',' opt_newlines type_bracket_item {
        $$ = $1;
        $$->push_back($4);
    }
    ;

single_type
    : dot_like_name {
        if (auto *field = dynamic_cast<AstField *>($1);
            field && field->name == string("any")) {
            $$ = new AnyTypeNode(@$);
        } else {
            $$ = new BaseTypeNode($1, @$);
        }
    }
    | TYPE { $$ = new BaseTypeNode($1->text, @$); }
    | dot_like_name '<' opt_newlines type_name_seq opt_newlines '>' {
        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Syntax, @$,
            "generic apply uses `[...]`, not `<>`",
            "Write `Name[T]` instead of `Name<T>` in type strings.");
    }
    | TYPE '<' opt_newlines type_name_seq opt_newlines '>' {
        throw lona::DiagnosticError(
            lona::DiagnosticError::Category::Syntax, @$,
            "generic apply uses `[...]`, not `<>`",
            "Write `Name[T]` instead of `Name<T>` in type strings.");
    }
    ;

tuple_type
    : '<' opt_newlines type_name_seq opt_newlines '>' {
        $$ = new TupleTypeNode(*$3, @$);
        delete $3;
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

postfix_type
    : type_primary { $$ = $1; }
    | postfix_type '*' %prec type_suffix { $$ = new PointerTypeNode($1, 1, @$); }
    | postfix_type '[' opt_newlines '*' opt_newlines ']' %prec type_suffix {
        $$ = new IndexablePointerTypeNode($1, @$);
    }
    | postfix_type '[' opt_newlines ']' %prec type_suffix { $$ = new ArrayTypeNode($1, {}, @$); }
    | postfix_type '[' opt_newlines type_bracket_item_seq opt_newlines ']' %prec type_suffix {
        $$ = createBracketSuffixTypeNode($1, $4, @$);
        delete $4;
    }
    | postfix_type '[' opt_newlines ',' opt_newlines expr_seq opt_newlines ']' %prec type_suffix {
        auto dims = *$6;
        dims.insert(dims.begin(), (AstNode*)nullptr);
        $$ = new ArrayTypeNode($1, dims, @$);
        delete $6;
    }
    | postfix_type DYN %prec type_suffix { $$ = new DynTypeNode($1, @$); }
    | postfix_type TYPE_CONST %prec type_suffix { $$ = new ConstTypeNode($1, @$); }
    ;

type_name
    : postfix_type { $$ = $1; }
    ;

func_ptr_type
    : '(' opt_newlines ':' opt_newlines ')' { $$ = new FuncPtrTypeNode({}, nullptr, @$); }
    | '(' opt_newlines ':' opt_newlines type_name opt_newlines ')' {
        $$ = new FuncPtrTypeNode({}, $5, @$);
    }
    | '(' opt_newlines func_param_type_seq opt_newlines ':' opt_newlines ')' {
        $$ = new FuncPtrTypeNode(*$3, nullptr, @$);
        delete $3;
    }
    | '(' opt_newlines func_param_type_seq opt_newlines ':' opt_newlines type_name opt_newlines ')' {
        $$ = new FuncPtrTypeNode(*$3, $7, @$);
        delete $3;
    }
    ;
