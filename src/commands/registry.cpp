#include "registry.h"

#include "json_out.h"

#include <algorithm>

namespace tgcurl {

// Command entry points, implemented one file per command in src/commands/*.
namespace commands {
std::optional<Error> login(const Args& args, std::ostream& out);
std::optional<Error> logout(const Args& args, std::ostream& out);
std::optional<Error> status(const Args& args, std::ostream& out);
std::optional<Error> contacts(const Args& args, std::ostream& out);
std::optional<Error> chats(const Args& args, std::ostream& out);
std::optional<Error> chat(const Args& args, std::ostream& out);
std::optional<Error> send(const Args& args, std::ostream& out);
std::optional<Error> search(const Args& args, std::ostream& out);
} // namespace commands

namespace {

constexpr const char* kIdDescription =
    "chat_id (from contacts_list / chats_list) or a public @username";

std::vector<CommandSpec> make_registry() {
    std::vector<CommandSpec> specs;

    // The session-lifecycle commands are CLI-only (tool = ""): login prompts
    // for phone/code/2FA on a TTY, which no head-less front-end can satisfy;
    // logout destroys the session every front-end depends on and only a human
    // (re-running login) can restore it; status is the human-side diagnostic
    // for that lifecycle. See DESIGN.md -> MCP mode -> CLI-only commands.
    specs.push_back({"login",
                     "",
                     "",
                     "Authenticate this device interactively (phone/code/2FA); idempotent",
                     {},
                     commands::login});

    specs.push_back({"logout",
                     "",
                     "",
                     "End the Telegram session and clear the local session database",
                     {},
                     commands::logout});

    specs.push_back({"status",
                     "",
                     "",
                     "Report whether a usable session exists and, if so, the account it "
                     "belongs to; never prompts",
                     {},
                     commands::status});

    specs.push_back({"contacts",
                     "list",
                     "contacts_list",
                     "List all address-book contacts as "
                     "{user_id, chat_id, username, phone, first_name, last_name}; "
                     "chat_id is the identifier every other command consumes",
                     {},
                     commands::contacts});

    specs.push_back({"contacts",
                     "new",
                     "contacts_new",
                     "Add a contact by phone number; returns {ok, user_id, chat_id} "
                     "(user_id 0 if the phone is not on Telegram)",
                     {
                         {"phone", ParamSpec::Type::String, true,
                          "phone number in international format, e.g. +31612345678", ""},
                         {"first_name", ParamSpec::Type::String, true, "contact first name", ""},
                         {"last_name", ParamSpec::Type::String, false, "contact last name", ""},
                     },
                     commands::contacts});

    specs.push_back({"contacts",
                     "block",
                     "contacts_block",
                     "Block a user or chat",
                     {
                         {"id", ParamSpec::Type::String, true, kIdDescription, ""},
                     },
                     commands::contacts});

    specs.push_back({"chats",
                     "list",
                     "chats_list",
                     "List recent dialogs (private chats, groups, channels) as {chat_id, "
                     "title, type, username, unread_count, last_message}; set unread=true to "
                     "get only chats with something new — the 'what needs attention' primitive",
                     {
                         {"limit", ParamSpec::Type::Integer, false,
                          "maximum number of chats to return (default 50)", "--limit"},
                         {"unread", ParamSpec::Type::Boolean, false,
                          "only chats with unread messages (or marked unread)", "--unread"},
                     },
                     commands::chats});

    specs.push_back({"chat",
                     "",
                     "chat_history",
                     "Read the most recent messages of a chat, newest first, as {id, date, "
                     "is_outgoing, sender_id, type, text, reply_to_message_id}; type tags the "
                     "content (text/photo/voice_note/...), text is the text or media caption",
                     {
                         {"id", ParamSpec::Type::String, true, kIdDescription, ""},
                         {"last", ParamSpec::Type::Integer, false,
                          "number of messages to return (default 20, max 100)", "--last"},
                     },
                     commands::chat});

    specs.push_back({"search",
                     "",
                     "search_messages",
                     "Search messages by text — inside one chat (chat_id given) or across all "
                     "chats (chat_id omitted); returns {total_count, messages:[{id, chat_id, "
                     "date, is_outgoing, sender_id, type, text, reply_to_message_id}]}, newest "
                     "first",
                     {
                         {"query", ParamSpec::Type::String, true, "text to search for", ""},
                         {"chat_id", ParamSpec::Type::String, false,
                          "limit the search to this chat: chat_id or a public @username", "--chat"},
                         {"limit", ParamSpec::Type::Integer, false,
                          "maximum number of messages to return (default 20, max 100)", "--limit"},
                     },
                     commands::search});

    specs.push_back(
        {"send",
         "",
         "send_message",
         "Send a text message to a chat, optionally as a reply; returns {ok, "
         "message_id, chat_id} only after the server has accepted the message",
         {
             {"id", ParamSpec::Type::String, true, kIdDescription, ""},
             {"text", ParamSpec::Type::String, true, "message text to send", ""},
             {"reply_to_message_id", ParamSpec::Type::Integer, false,
              "message id (from chat_history/search_messages) to reply to", "--reply-to"},
         },
         commands::send});

    return specs;
}

} // namespace

const std::vector<CommandSpec>& registry() {
    static const std::vector<CommandSpec> specs = make_registry();
    return specs;
}

namespace {

const std::string* find_named(const std::vector<std::pair<std::string, std::string>>& named,
                              const std::string& name) {
    for (const auto& [n, v] : named) {
        if (n == name) {
            return &v;
        }
    }
    return nullptr;
}

// Append the positional params (declaration order). Once an optional
// positional is omitted, no later positional may be present — there is no way
// to express the gap on a CLI.
std::optional<Error>
append_positionals(const CommandSpec& spec,
                   const std::vector<std::pair<std::string, std::string>>& named, Args& args) {
    bool gap = false;
    for (const ParamSpec& p : spec.params) {
        if (!p.flag.empty()) {
            continue;
        }
        const std::string* value = find_named(named, p.name);
        if (value == nullptr) {
            if (p.required) {
                return Error("invalid_params", "missing required argument: " + p.name);
            }
            gap = true;
            continue;
        }
        if (gap) {
            return Error("invalid_params",
                         "argument " + p.name + " requires the earlier optional ones too");
        }
        args.push_back(*value);
    }
    return std::nullopt;
}

// Append the flag params: "--flag value" pairs, or the bare flag for a
// Boolean set to true (false/absent adds nothing).
std::optional<Error> append_flags(const CommandSpec& spec,
                                  const std::vector<std::pair<std::string, std::string>>& named,
                                  Args& args) {
    for (const ParamSpec& p : spec.params) {
        if (p.flag.empty()) {
            continue;
        }
        const std::string* value = find_named(named, p.name);
        if (value == nullptr) {
            if (p.required) {
                return Error("invalid_params", "missing required argument: " + p.name);
            }
            continue;
        }
        if (p.type == ParamSpec::Type::Boolean) {
            if (*value == "true") {
                args.push_back(p.flag);
            } else if (*value != "false") {
                return Error("invalid_params", "argument " + p.name + " must be a boolean");
            }
            continue;
        }
        args.push_back(p.flag);
        args.push_back(*value);
    }
    return std::nullopt;
}

} // namespace

std::variant<Args, Error>
build_cli_args(const CommandSpec& spec,
               const std::vector<std::pair<std::string, std::string>>& named) {
    // Reject argument names the spec doesn't declare — typos should fail
    // loudly, not be silently dropped.
    for (const auto& [name, value] : named) {
        const bool known =
            std::any_of(spec.params.begin(), spec.params.end(),
                        [&name = name](const ParamSpec& p) { return p.name == name; });
        if (!known) {
            return Error("invalid_params", "unknown argument: " + name);
        }
    }

    Args args;
    if (!spec.subcommand.empty()) {
        args.push_back(spec.subcommand);
    }
    if (std::optional<Error> err = append_positionals(spec, named, args)) {
        return *err;
    }
    if (std::optional<Error> err = append_flags(spec, named, args)) {
        return *err;
    }
    return args;
}

std::string input_schema_json(const CommandSpec& spec) {
    json::Writer properties;
    json::ArrayWriter required;
    for (const ParamSpec& p : spec.params) {
        json::Writer prop;
        const char* type_name = "string";
        if (p.type == ParamSpec::Type::Integer) {
            type_name = "integer";
        } else if (p.type == ParamSpec::Type::Boolean) {
            type_name = "boolean";
        }
        prop.field("type", type_name);
        prop.field("description", p.description);
        properties.raw_field(p.name, prop.object());
        if (p.required) {
            required.element(json::quote(p.name));
        }
    }

    json::Writer schema;
    schema.field("type", "object");
    schema.raw_field("properties", properties.object());
    schema.raw_field("required", required.array());
    return schema.object();
}

} // namespace tgcurl
