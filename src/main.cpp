// tgcurl entry point: parse the subcommand and dispatch.
//
// Every command is one-shot and emits JSON. Any handled failure is printed as
// a JSON error on stderr with a non-zero exit code (see error.h). The dispatch
// table is derived from the command registry (src/commands/registry.cpp) — the
// same single source of truth the MCP server exposes as tools, so the two
// front-ends can't drift.
#include "commands/registry.h"
#include "error.h"
#include "mcp.h"

#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tgcurl {
namespace {

// Unique top-level command word -> handler, derived from the registry (several
// specs share one word via subcommands, e.g. `contacts list|new|block`).
std::map<std::string, Handler> dispatch_table() {
    std::map<std::string, Handler> table;
    for (const CommandSpec& spec : registry()) {
        table.emplace(spec.command, spec.handler);
    }
    return table;
}

std::string usage_line(const std::map<std::string, Handler>& table) {
    std::string names;
    for (const auto& [name, handler] : table) {
        if (!names.empty()) {
            names += ", ";
        }
        names += name;
    }
    return "expected a subcommand: " + names + " (or -mcp to serve MCP over stdio)";
}

int run(const Args& argv) {
    // -mcp turns the process into an MCP stdio server exposing the same
    // registry as tools; everything else is one-shot CLI dispatch.
    if (!argv.empty() && (argv[0] == "-mcp" || argv[0] == "--mcp")) {
        return mcp::serve(std::cin, std::cout);
    }

    const std::map<std::string, Handler> table = dispatch_table();

    if (argv.empty()) {
        emit_error(Error("usage", usage_line(table)), std::cerr);
        return kExitError;
    }

    const std::string& cmd = argv[0];
    auto it = table.find(cmd);
    if (it == table.end()) {
        emit_error(Error("unknown_command", "no such command: '" + cmd + "'"), std::cerr);
        return kExitError;
    }

    const Args rest(argv.begin() + 1, argv.end());
    if (std::optional<Error> err = it->second(rest, std::cout)) {
        emit_error(*err, std::cerr);
        return kExitError;
    }
    return 0;
}

} // namespace
} // namespace tgcurl

int main(int argc, char** argv) {
    // Collect argv[1..] as the command + its arguments. Index-based to avoid
    // raw pointer arithmetic; argv[0] (the program name) is skipped.
    tgcurl::Args args;
    for (int i = 1; i < argc; ++i) {
        // argv indexing at the C boundary; bounded by argc.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        args.emplace_back(argv[i]);
    }
    return tgcurl::run(args);
}
