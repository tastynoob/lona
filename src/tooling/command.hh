#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lona::tooling {

class Session;
class OutputFormatter;
class CommandRegistry;

struct CommandOutcome {
    bool keepRunning = true;
    int exitCode = 0;
};

struct ParsedCommand {
    std::string raw;
    std::string name;
    std::string args;
};

enum class CommandArgumentPolicy {
    None,
    Optional,
    Required,
};

using CommandHandler = std::function<CommandOutcome(
    Session &, const ParsedCommand &, OutputFormatter &,
    const CommandRegistry &)>;

struct CommandSpec {
    std::string name;
    std::string usage;
    std::string description;
    CommandArgumentPolicy argumentPolicy = CommandArgumentPolicy::None;
    bool hidden = false;
    CommandHandler handler;
};

class CommandRegistry {
    std::vector<CommandSpec> commands_;
    std::unordered_map<std::string, std::size_t> indexByName_;

    std::optional<ParsedCommand> parse(std::string_view raw) const;
    const CommandSpec *find(std::string_view name) const;

public:
    void add(CommandSpec spec);
    CommandOutcome dispatch(Session &session, std::string_view raw,
                            OutputFormatter &formatter) const;
    std::vector<const CommandSpec *> visibleCommands() const;
};

CommandRegistry buildCommandRegistry();

std::string trimCopy(std::string_view text);
std::optional<int> parsePositiveInt(std::string_view text);
std::pair<std::string, std::string> parseFilterAndPattern(std::string_view text);

}  // namespace lona::tooling
