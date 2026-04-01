#include "lona/ast/astnode.hh"
#include "lona/ast/type_node_string.hh"
#include "lona/declare/support.hh"
#include "lona/err/err.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/type/buildin.hh"
#include "lona/type/scope.hh"
#include "lona/visitor.hh"
#include <cassert>
#include <list>
#include <string>
#include <unordered_map>

namespace lona {

using declarationsupport_impl::declareFunction;
using declarationsupport_impl::declareStructType;
using declarationsupport_impl::describeStructFieldSyntax;
using declarationsupport_impl::insertStructMember;
using declarationsupport_impl::recordTopLevelDeclName;
using declarationsupport_impl::rejectBareFunctionType;
using declarationsupport_impl::requireTypeTable;
using declarationsupport_impl::resolveTypeNode;
using declarationsupport_impl::TopLevelDeclKind;
using declarationsupport_impl::validateEmbeddedStructField;
using declarationsupport_impl::validateStructDeclShape;
using declarationsupport_impl::validateStructFieldType;

class StructVisitor : public AstVisitorAny {
    TypeTable *typeMgr;
    CompilationUnit *unit;
    bool exportNamespace;
    AstStructDecl *structDecl = nullptr;

    llvm::StringMap<StructType::ValueTy> members;
    llvm::StringMap<AccessKind> memberAccess;
    llvm::StringSet<> embeddedMembers;
    std::unordered_map<std::string, location> seenMembers;
    int nextMemberIndex = 0;

    using AstVisitorAny::visit;

    Object *visit(AstStatList *node) override {
        for (auto it = node->getBody().begin(); it != node->getBody().end();
             it++) {
            (*it)->accept(*this);
        }
        return nullptr;
    }

    Object *visit(AstVarDecl *node) override {
        if (node->bindingKind == BindingKind::Ref) {
            error(node->loc,
                  "struct fields cannot use `ref` binding for `" +
                      describeStructFieldSyntax(node) + "`",
                  "Store an explicit pointer type instead. Struct fields must "
                  "be value or pointer-like storage.");
        }
        auto *type = resolveTypeNode(typeMgr, unit, node->typeNode);
        if (!type) {
            error(node->loc, "unknown struct field type for `" +
                                 describeStructFieldSyntax(node) + "`: " +
                                 describeTypeNode(node->typeNode, "void"));
        }
        rejectBareFunctionType(
            type, node->typeNode,
            "unsupported bare function struct field type for `" +
                describeStructFieldSyntax(node) + "`",
            node->loc);
        validateStructFieldType(structDecl, node, type);
        validateEmbeddedStructField(structDecl, node, type);
        insertStructMember(structDecl, node, type, members, memberAccess,
                           embeddedMembers, seenMembers, nextMemberIndex);

        return nullptr;
    }

public:
    StructVisitor(TypeTable *typeMgr, AstStructDecl *node,
                  CompilationUnit *unit = nullptr, bool exportNamespace = false)
        : typeMgr(typeMgr),
          unit(unit),
          exportNamespace(exportNamespace),
          structDecl(node) {
        auto *lostructTy =
            declareStructType(typeMgr, node, unit, exportNamespace);
        assert(lostructTy);

        if (!lostructTy->isOpaque()) {
            return;
        }

        if (!node->body) {
            return;
        }

        this->visit(node->body);
        lostructTy->complete(members, memberAccess, embeddedMembers);
    }
};

class TypeCollector : public AstVisitorAny {
    TypeTable *typeMgr;
    Scope *scope;
    CompilationUnit *unit;
    bool exportNamespace;

    std::list<AstStructDecl *> structDecls;
    std::list<AstFuncDecl *> funcDecls;
    std::unordered_map<std::string, std::pair<TopLevelDeclKind, location>>
        topLevelDecls;

    using AstVisitorAny::visit;

    Object *visit(AstProgram *node) override {
        this->visit(node->body);
        return nullptr;
    }

    Object *visit(AstStatList *node) override {
        for (auto *it : node->body) {
            if (it->is<AstStructDecl>()) {
                auto *decl = it->as<AstStructDecl>();
                validateStructDeclShape(decl);
                recordTopLevelDeclName(topLevelDecls, toStdString(decl->name),
                                       TopLevelDeclKind::StructType, decl->loc);
                structDecls.push_back(it->as<AstStructDecl>());
            } else if (it->is<AstTraitDecl>()) {
                auto *decl = it->as<AstTraitDecl>();
                recordTopLevelDeclName(topLevelDecls, toStdString(decl->name),
                                       TopLevelDeclKind::Trait, decl->loc);
            } else if (it->is<AstFuncDecl>()) {
                auto *decl = it->as<AstFuncDecl>();
                recordTopLevelDeclName(topLevelDecls, toStdString(decl->name),
                                       TopLevelDeclKind::Function, decl->loc);
                funcDecls.push_back(it->as<AstFuncDecl>());
            }
        }
        return nullptr;
    }

    Object *visit(AstStructDecl *node) override {
        if (node->hasTypeParams()) {
            return nullptr;
        }
        auto *structTy =
            declareStructType(typeMgr, node, unit, exportNamespace);
        if (!node->body || !node->body->is<AstStatList>()) {
            return nullptr;
        }
        for (auto *stmt : node->body->as<AstStatList>()->getBody()) {
            auto *func = stmt->as<AstFuncDecl>();
            if (!func) {
                continue;
            }
            declareFunction(*scope, typeMgr, func, structTy, unit,
                            exportNamespace);
        }
        return nullptr;
    }

    Object *visit(AstFuncDecl *node) override {
        if (node->hasTypeParams()) {
            return nullptr;
        }
        declareFunction(*scope, typeMgr, node, nullptr, unit, exportNamespace);
        return nullptr;
    }

public:
    TypeCollector(TypeTable *typeMgr, Scope *scope, AstNode *root,
                  CompilationUnit *unit = nullptr, bool exportNamespace = false)
        : typeMgr(typeMgr),
          scope(scope),
          unit(unit),
          exportNamespace(exportNamespace) {
        this->visit(root);

        for (auto *it : structDecls) {
            declareStructType(typeMgr, it, unit, exportNamespace);
        }

        for (auto *it : structDecls) {
            if (!it->hasTypeParams()) {
                StructVisitor(typeMgr, it, unit, exportNamespace);
            }
        }

        for (auto *it : structDecls) {
            this->visit(it);
        }

        for (auto *it : funcDecls) {
            this->visit(it);
        }
    }
};

Function *
createFunc(Scope &scope, AstFuncDecl *root, StructType *parent) {
    initBuildinType(&scope);
    auto *func = declareFunction(scope, requireTypeTable(&scope), root, parent,
                                 nullptr, false);
    return func;
}

void
scanningType(Scope *global, AstNode *root) {
    initBuildinType(global);
    TypeCollector(requireTypeTable(global), global, root, nullptr, false);
}

StructType *
createStruct(Scope *scope, AstStructDecl *node) {
    initBuildinType(scope);
    auto *typeMgr = requireTypeTable(scope);
    auto *type = declareStructType(typeMgr, node, nullptr, false);
    if (type->isOpaque()) {
        StructVisitor(typeMgr, node, nullptr, false);
    }
    return type;
}

}  // namespace lona
