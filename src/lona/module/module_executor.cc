#include "module_executor.hh"
#include <utility>

namespace lona {

int
SerialModuleExecutor::execute(ModuleBuildQueue &queue, BuildTask task) {
    while (!queue.empty()) {
        auto path = queue.popNext();
        int exitCode = task(path);
        if (exitCode != 0) {
            return exitCode;
        }
    }
    return 0;
}

std::unique_ptr<ModuleExecutor>
createSerialModuleExecutor() {
    return std::make_unique<SerialModuleExecutor>();
}

}  // namespace lona
