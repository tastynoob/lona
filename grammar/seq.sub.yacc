field_seq
    : FIELD  {}
    | field_seq ',' FIELD {}
    ;

expr_seq
    : expr {}
    | expr_seq ',' expr {}
    ;

var_decl_seq
    : var_decl {}
    | var_decl_seq ',' var_decl {}
    ;

type_name_seq
    : type_name {}
    | type_name_seq ',' type_name {}
    ;