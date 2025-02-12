// #include "base.hh"

// namespace lona
// {

// // remove dead blocks
// // end else block merge
// class BlockOptimizer : public AstVisitor
// {
//   public:
//     using AstVisitor::visit;

//     void visit(AstFuncDecl* node) override { node->iterateChild(*this); }

//     void visit(AstIf* node) override
//     {
//         bool thenEmpty = node->getThen()->is<AstStatList>() &&
//                          node->getThen()->as<AstStatList>().isEmpty();
//         bool elseEmpty = node->getElse() &&
//                          node->getElse()->is<AstStatList>() &&
//                          node->getElse()->as<AstStatList>().isEmpty();

//         if (thenEmpty && elseEmpty) {
//             node->setEmpty();
//             return;
//         }

//         node->iterateChild(*this);
//     }

//     void visit(AstStatList* node) override
//     {
//         auto& body = node->getBody();
//         for (auto it = body.begin(); it != body.end();) {
//             AstNode* stat = *it;
//             bool lastStat = it == (--body.end());
//             if (stat->is<AstIf>()) {

//                 auto& ifstat = stat->as<AstIf>();
//                 AstNode* els = ifstat.getElse();

//                 if (ifstat.isEmpty()) {
//                     it = body.erase(it);
//                     delete stat;
//                 }

//                 if (ifstat.getThen()->is<AstStatList>() &&
//                     ifstat.getThen()->as<AstStatList>().isEmpty()) {
//                     // transform if (a) {} else { B } => if (!a) { B }
//                     ifstat.setCondition(nullptr);
//                     throw std::runtime_error("not implemented");
//                     ifstat.setThen(els);
//                     ifstat.setElse(nullptr);
//                 }
//             }

//             it++;
//         }
//     }
// };

// }