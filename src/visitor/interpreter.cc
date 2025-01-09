#include "base.hh"
#include <stack>
#include <unordered_map>

namespace lona {

// class Interpreter : public AstVisitor
// {      // runtime stack
// public:
//     void printResult(std::ostream& os)
//     {

//     }

//     using AstVisitor::visit;

//     void visit(AstConst* node) override
//     {

//     }
//     void visit(AstField* node) override
//     {

//     }
//     void visit(AstBinOper* node) override
//     {
//         node->iterateChild(*this);
//         auto right = std::move(rStack.top());
//         rStack.pop();
//         auto left = std::move(rStack.top());
//         rStack.pop();

//         switch (node->getOp()) {
//             case SymbolTable::ADD:
//                 // rStack.push(left + right);
//                 break;
//             case SymbolTable::SUB:
//                 // rStack.push(left - right);
//                 break;
//             case SymbolTable::MUL:
//                 // rStack.push(left * right);
//                 break;
//             default:
//                 throw std::runtime_error("Invalid operator");
//                 break;
//         }
//     }

//     void visit(AstVarDecl* node) override
//     {
//         node->iterateChild(*this);
//         varlist.insert({node->getField(), std::move(rStack.top())});
//         rStack.pop();
//     }
//     void visit(AstStatList* node) override { node->iterateChild(*this); }

//     void visit(AstFuncDecl* node) override {}
// };

// void
// interpreter(AstNode* node)
// {
//     Interpreter interpreter;
//     interpreter.visit(node);
//     interpreter.printResult(std::cout);
// }

}  // namespace lona