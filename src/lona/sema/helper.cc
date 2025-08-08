#include "../visitor.hh"
#include "lona/ast/astnode.hh"
#include "lona/obj/value.hh"
#include "lona/sema/hir.hh"
#include "lona/type/buildin.hh"
#include "parser.hh"




namespace lona {




HIRNode* shortCircuitTransform(ObjectPtr res, ObjectPtr left, ObjectPtr right, bool isAnd) {
    // x = a && b => if (a) {x = b;} else {x = false;}
    // x = a || b => if (a) {x = true;} else {x = b;}
    auto hirIf = new HIRIf(left);

    if (isAnd) {
        hirIf->pushThenNode(new HIRAssign(res, right));
        hirIf->pushElseNode(new HIRAssign(res, new ConstVar(boolTy, false)));
    } else {
        hirIf->pushThenNode(new HIRAssign(res, new ConstVar(boolTy, true)));
        hirIf->pushElseNode(new HIRAssign(res, right));
    }

    return hirIf;
}





}