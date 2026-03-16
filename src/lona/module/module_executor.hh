#pragma once

#include "build_queue.hh"
#include <functional>
#include <memory>
#include <string>

namespace lona {

class ModuleExecutor {
public:
    using BuildTask = std::function<int(const std::string &)>;

    virtual ~ModuleExecutor() = default;
    virtual int execute(ModuleBuildQueue &queue, BuildTask task) = 0;
};

class SerialModuleExecutor final : public ModuleExecutor {
public:
    int execute(ModuleBuildQueue &queue, BuildTask task) override;
};

std::unique_ptr<ModuleExecutor> createSerialModuleExecutor();

}  // namespace lona
