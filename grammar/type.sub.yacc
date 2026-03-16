/* single_type
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
   ; */

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

ptr_type
    : base_type '*' { $$ = new PointerTypeNode($1, 1, @$); }
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
    | array_type { $$ = $1; }
    ;

func_head
    : '(' ')' { $$ = new FuncTypeNode({}, nullptr, @$); }
    | '(' type_name_seq ')' { $$ = new FuncTypeNode(*$2, nullptr, @$); delete $2; }
    | func_head '*' { $$ = new PointerTypeNode($1, 1, @$); }
    | func_head '[' ']' { $$ = new ArrayTypeNode($1, {}, @$); }
    | func_head '[' expr_seq ']' {
        $$ = new ArrayTypeNode($1, *$3, @$);
        delete $3;
    }
    | func_head '[' ',' expr_seq ']' {
        auto dims = *$4;
        dims.insert(dims.begin(), (AstNode*)nullptr);
        $$ = new ArrayTypeNode($1, dims, @$);
        delete $4;
    }
    ;

type_name
    : base_type { $$ = $1; }
    | func_head type_name {
        $$ = $1;
        if (auto *func = findFuncTypeNode($1)) {
            func->ret = $2;
        }
    }
    ;
