#include "lona/analyze/function.hh"
#include "lona/module/compilation_unit.hh"
#include "lona/resolve/resolve.hh"
#include "lona/sema/hir.hh"
#include <memory>

namespace lona {
namespace analysis_impl {

class ModuleAnalyzer {
    GlobalScope *global;
    const CompilationUnit *unit;
    std::unique_ptr<HIRModule> module = std::make_unique<HIRModule>();

public:
    explicit ModuleAnalyzer(GlobalScope *global, const CompilationUnit *unit)
        : global(global), unit(unit) {}

    std::unique_ptr<HIRModule> analyze(const ResolvedModule &resolvedModule) {
        for (const auto &resolvedFunction : resolvedModule.functions()) {
            if (resolvedFunction->isTemplateValidationOnly()) {
                continue;
            }
            module->addFunction(analyzeResolvedFunction(
                global, module.get(), unit, *resolvedFunction));
        }
        return std::move(module);
    }
};

}  // namespace analysis_impl

std::unique_ptr<HIRModule>
analyzeModule(GlobalScope *global, const ResolvedModule &resolved,
              const CompilationUnit *unit) {
    return analysis_impl::ModuleAnalyzer(global, unit).analyze(resolved);
}

}  // namespace lona
