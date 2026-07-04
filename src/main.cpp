// tgcurl entry point: parse the subcommand and dispatch.
//
// Every command is one-shot and emits JSON. Any handled failure is printed as
// a JSON error on stderr with a non-zero exit code (see error.h). Individual
// commands are implemented in src/commands/*; at this scaffold stage they are
// stubs returning `not_implemented`.
#include "error.h"
#include "json_out.h"

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
// Stubs for now; each is implemented under its own issue.
std::optional<Error> login(const Args&);
std::optional<Error> logout(const Args&);
std::optional<Error> contacts(const Args&);
std::optional<Error> chats(const Args&);
std::optional<Error> chat(const Args&);
std::optional<Error> send(const Args&);
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

// --- Command stubs (temporary; replaced in later issues) --------------------
namespace tgcurl::commands {
std::optional<Error> login(const Args&) {
    return Error("not_implemented", "login: see issue #5");
}
std::optional<Error> logout(const Args&) {
    return Error("not_implemented", "logout: see issue #5");
}
std::optional<Error> contacts(const Args&) {
    return Error("not_implemented", "contacts: see issue #6");
}
std::optional<Error> chats(const Args&) {
    return Error("not_implemented", "chats: see issue #6");
}
std::optional<Error> chat(const Args&) {
    return Error("not_implemented", "chat: see issue #7");
}
std::optional<Error> send(const Args&) {
    return Error("not_implemented", "send: see issue #7");
}
} // namespace tgcurl::commands

int main(int argc, char** argv) {
    tgcurl::Args args(argv + (argc > 0 ? 1 : 0), argv + argc);
    return tgcurl::run(args);
}
