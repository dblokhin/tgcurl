// The single command table driving BOTH front-ends.
//
// tgcurl exposes every capability twice: as a CLI subcommand and as an MCP
// tool (DESIGN.md → MCP mode). To keep the two from drifting, a command is
// declared exactly once here — its CLI shape (command word + optional
// subcommand), its MCP tool name/description/parameters, and the handler.
// main.cpp derives its dispatch table from this registry; the MCP server
// derives tools/list schemas and tools/call argument mapping from it. Adding
// a new command = implementing the handler and appending one CommandSpec;
// both front-ends pick it up automatically.
#ifndef TGCURL_COMMANDS_REGISTRY_H
#define TGCURL_COMMANDS_REGISTRY_H

#include "error.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tgcurl {

using Args = std::vector<std::string>;

// A handler is a top-level command entry point: it receives the arguments
// after the command word (including any subcommand word), writes its JSON
// result to `out` on success, or returns an Error.
using Handler = std::function<std::optional<Error>(const Args&, std::ostream&)>;

// One MCP-visible parameter of a command and how it maps onto CLI arguments.
struct ParamSpec {
    enum class Type : std::uint8_t { String, Integer, Boolean };

    std::string name; // MCP argument name, e.g. "chat_id"
    Type type = Type::String;
    bool required = true;
    std::string description; // shown to agents in the tool's inputSchema
    // How the value reaches the CLI arg vector: "" = positional (in ParamSpec
    // declaration order); otherwise the flag it follows, e.g. "--limit". A
    // Boolean param must be an optional flag: true -> the bare flag is
    // appended (no value), false/absent -> nothing.
    std::string flag;
};

// One command, declared once for both front-ends.
struct CommandSpec {
    std::string command;    // top-level CLI word, e.g. "contacts"
    std::string subcommand; // CLI subcommand word, "" if none
    // MCP tool name, e.g. "contacts_list"; "" = CLI-only (login: it must
    // prompt on a TTY, which a head-less MCP server cannot do).
    std::string tool;
    std::string description; // one-liner for agents (tools/list) and usage
    std::vector<ParamSpec> params;
    Handler handler;
};

// The registry, in stable declaration order.
const std::vector<CommandSpec>& registry();

// Build the CLI argument vector the spec's handler expects from MCP-style
// named arguments (already stringified by the caller): subcommand word, then
// positional params in declaration order, then flag params. Errors
// ("invalid_params") on a missing required argument, an unknown argument
// name, or a present positional after an omitted optional one.
std::variant<Args, Error>
build_cli_args(const CommandSpec& spec,
               const std::vector<std::pair<std::string, std::string>>& named);

// The MCP inputSchema JSON for a spec's parameters:
// {"type":"object","properties":{...},"required":[...]}.
std::string input_schema_json(const CommandSpec& spec);

} // namespace tgcurl

#endif // TGCURL_COMMANDS_REGISTRY_H
