// login / logout commands.
//
// login runs the authorization flow (interactively on a TTY) and, if it isn't
// already authorized, prompts for the credentials it needs; on success it
// prints the authenticated user. logout ends the session and clears the local
// database. Both are the only commands allowed to touch stdin.
#include "auth.h"
#include "config.h"
#include "error.h"
#include "json_out.h"
#include "tdclient.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <td/telegram/td_api.h>
#include <unistd.h>
#include <variant>
#include <vector>

namespace tgcurl {

using Args = std::vector<std::string>;

namespace td_api = td::td_api;

namespace {

// Read one trimmed line from stdin. Returns nullopt on EOF.
std::optional<std::string> read_line() {
    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }
    // Trim surrounding whitespace (users paste with stray spaces/newlines).
    const auto begin = line.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const auto end = line.find_last_not_of(" \t\r\n");
    return line.substr(begin, end - begin + 1);
}

// Prompt to stderr (stdout is reserved for the JSON result) and read a line.
std::optional<std::string> ask(const std::string& label) {
    std::cerr << label << std::flush;
    return read_line();
}

// A TTY-backed prompter for the interactive login flow.
class TtyPrompter final : public Prompter {
  public:
    [[nodiscard]] bool interactive() const override { return true; }
    std::optional<std::string> prompt_phone() override {
        return ask("Phone number (international, e.g. +123...): ");
    }
    std::optional<std::string> prompt_code() override { return ask("Login code: "); }
    std::optional<std::string> prompt_password(const std::string& hint) override {
        std::string label = "2FA password";
        if (!hint.empty()) {
            label += " (hint: " + hint + ")";
        }
        label += ": ";
        return ask(label);
    }
};

// Load the config, prompting for and saving api_id/api_hash if it's absent.
// Only usable interactively (login). Returns the config or an Error.
std::variant<Config, Error> load_or_create_config() {
    std::variant<Config, Error> loaded = load_config();
    if (std::holds_alternative<Config>(loaded)) {
        return loaded;
    }
    // Only "config_missing" is recoverable by prompting; a malformed file is a
    // hard error the user must fix.
    const Error& err = std::get<Error>(loaded);
    if (err.code() != "config_missing") {
        return err;
    }

    std::cerr << "No API credentials found. Get them from https://my.telegram.org/apps\n";
    std::optional<std::string> id_str = ask("api_id: ");
    std::optional<std::string> hash = ask("api_hash: ");
    if (!id_str.has_value() || id_str->empty() || !hash.has_value() || hash->empty()) {
        return Error("auth_failed", "api_id/api_hash are required");
    }
    Config config;
    try {
        std::size_t consumed = 0;
        config.api_id = std::stoi(*id_str, &consumed);
        if (consumed != id_str->size() || config.api_id <= 0) {
            return Error("auth_failed", "api_id must be a positive integer");
        }
    } catch (const std::exception&) {
        return Error("auth_failed", "api_id must be a positive integer");
    }
    config.api_hash = *hash;

    if (std::optional<Error> save_err = save_config(config)) {
        return *save_err;
    }
    return config;
}

// Serialize a td_api::user into a JSON object string.
std::string user_json(const td_api::user& user) {
    json::Writer w;
    w.field("user_id", static_cast<std::int64_t>(user.id_));
    w.field("first_name", user.first_name_);
    w.field("last_name", user.last_name_);
    w.field("username", primary_username(user.usernames_));
    w.field("phone", user.phone_number_);
    return w.object();
}

// Fetch the authenticated user as JSON. Returns the JSON or an Error.
std::variant<std::string, Error> fetch_me(TdClient& client) {
    Object me = client.send_query(td_api::make_object<td_api::getMe>());
    if (is_error(me)) {
        return Error("auth_failed", "getMe: " + error_text(me));
    }
    // Safe downcast: getMe returns a user on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& user = static_cast<const td_api::user&>(*me);
    return user_json(user);
}

} // namespace

namespace commands {

std::optional<Error> login(const Args& /*args*/) {
    std::variant<Config, Error> cfg = load_or_create_config();
    if (std::holds_alternative<Error>(cfg)) {
        return std::get<Error>(cfg);
    }
    const Config& config = std::get<Config>(cfg);

    if (std::optional<Error> db_err = ensure_database_dir()) {
        return db_err;
    }

    TdClient client;
    TtyPrompter prompter;
    AuthResult result = authenticate(client, config, prompter);
    if (!result.authorized) {
        if (result.error.has_value()) {
            return result.error;
        }
        return Error("auth_failed", "login did not complete");
    }

    std::variant<std::string, Error> me = fetch_me(client);
    if (std::holds_alternative<Error>(me)) {
        return std::get<Error>(me);
    }

    json::Writer w;
    w.field("ok", true);
    w.raw_field("user", std::get<std::string>(me));
    w.field("already", result.already);
    json::emit(w.object(), std::cout);
    return std::nullopt;
}

std::optional<Error> logout(const Args& /*args*/) {
    std::variant<Config, Error> cfg = load_config();
    if (std::holds_alternative<Error>(cfg)) {
        // No config → nothing to log out of, but still clear any db remnants.
        std::error_code ec;
        std::filesystem::remove_all(database_dir(), ec);
        json::Writer w;
        w.field("ok", true);
        json::emit(w.object(), std::cout);
        return std::nullopt;
    }
    const Config& config = std::get<Config>(cfg);

    // Reach at least a parameters-set client so logOut can be issued. Use a
    // head-less prompter: if there's no session, there's nothing to log out —
    // we still clear the db and report ok.
    TdClient client;
    HeadlessPrompter prompter;
    AuthResult result = authenticate(client, config, prompter);
    if (result.authorized) {
        Object out = client.send_query(td_api::make_object<td_api::logOut>());
        if (is_error(out)) {
            return Error("auth_failed", "logOut: " + error_text(out));
        }
    }

    // Clear the local session database regardless.
    std::error_code ec;
    std::filesystem::remove_all(database_dir(), ec);

    json::Writer w;
    w.field("ok", true);
    json::emit(w.object(), std::cout);
    return std::nullopt;
}

} // namespace commands
} // namespace tgcurl
