#pragma once

#include "lona/module/compilation_unit.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/hir.hh"
#include "lona/type/scope.hh"
#include <unordered_map>

namespace lona {
namespace analysis_impl {

struct AnalysisLookupCache {
    struct VisibleTypeLookup {
        const ModuleInterface::TypeDecl *typeDecl = nullptr;
        const CompilationUnit *ownerUnit = nullptr;

        bool found() const { return typeDecl != nullptr && ownerUnit != nullptr; }
    };

    const CompilationUnit *rootUnit = nullptr;
    bool visibleTypesReady = false;
    std::unordered_map<StructType *, VisibleTypeLookup> visibleTypesByRuntimeType;
    std::unordered_map<string, VisibleTypeLookup> visibleTypesByExportedName;
    std::unordered_map<const CompilationUnit *,
                       std::unordered_map<string,
                                          const ModuleInterface::TypeDecl *>>
        typeDeclsByExportedName;
    std::unordered_map<const CompilationUnit *,
                       std::unordered_map<string, const AstStructDecl *>>
        structSyntaxByUnit;
    std::unordered_map<const AstStructDecl *,
                       std::unordered_map<string, const AstFuncDecl *>>
        methodSyntaxByStruct;

    explicit AnalysisLookupCache(const CompilationUnit *rootUnit = nullptr)
        : rootUnit(rootUnit) {}
};

HIRFunc *
analyzeResolvedFunction(GlobalScope *global, HIRModule *ownerModule,
                        const CompilationUnit *unit,
                        const ResolvedFunction &resolved,
                        AnalysisLookupCache *lookupCache = nullptr);

}  // namespace analysis_impl
}  // namespace lona
