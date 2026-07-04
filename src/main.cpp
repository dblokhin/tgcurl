// tgcurl entry point: parse the subcommand and dispatch.
//
// Every command is one-shot and emits JSON. Any handled failure is printed as
// a JSON error on stderr with a non-zero exit code (see error.h). Individual
// commands are implemented in src/commands/*; at this scaffold stage they are
// stubs returning `not_implemented`.
#include "error.h"

#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tgcurl {

// A command receives the arguments *after* its own name and returns either
// nothing (success — it printed its own JSON) or an Error.
using Args = std::vector<std::string>;

namespace commands {
// login / logout live in src/commands/auth_cmds.cpp; the rest are stubs until
// their issue lands.
std::optional<Error> login(const Args& args);
std::optional<Error> logout(const Args& args);
std::optional<Error> contacts(const Args& args);
std::optional<Error> chats(const Args& args);
std::optional<Error> chat(const Args& args);
std::optional<Error> send(const Args& args);
} // namespace commands

namespace {

int run(const Args& argv) {
    if (argv.empty()) {
        emit_error(Error("usage", "expected a subcommand: login, logout, contacts, chats, chat, "
                                  "send"),
                   std::cerr);
        return kExitError;
    }

    const std::string& cmd = argv[0];
    const Args rest(argv.begin() + 1, argv.end());

    static const std::map<std::string, std::function<std::optional<Error>(const Args&)>> table = {
        {"login", commands::login}, {"logout", commands::logout}, {"contacts", commands::contacts},
        {"chats", commands::chats}, {"chat", commands::chat},     {"send", commands::send},
    };

    auto it = table.find(cmd);
    if (it == table.end()) {
        emit_error(Error("unknown_command", "no such command: '" + cmd + "'"), std::cerr);
        return kExitError;
    }

    if (std::optional<Error> err = it->second(rest)) {
        emit_error(*err, std::cerr);
        return kExitError;
    }
    return 0;
}

} // namespace
} // namespace tgcurl

// All commands are implemented under src/commands/*:
//   login / logout          -> auth_cmds.cpp
//   contacts list           -> contacts.cpp
//   chats list              -> chats.cpp
//   chat / send             -> chat.cpp / send.cpp

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
