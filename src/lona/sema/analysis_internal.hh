#pragma once

#include "lona/module/compilation_unit.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/hir.hh"
#include "lona/type/scope.hh"

namespace lona {
namespace analysis_impl {

HIRFunc *
analyzeResolvedFunction(GlobalScope *global, HIRModule *ownerModule,
                        const CompilationUnit *unit,
                        const ResolvedFunction &resolved);

}  // namespace analysis_impl
}  // namespace lona
