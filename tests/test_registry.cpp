// Tests for the command registry — the single table both front-ends (CLI
// dispatch and MCP tools) are derived from. Covers the spec invariants, the
// MCP inputSchema generation, and the named-argument -> CLI-argv mapping.
#include "commands/registry.h"
#include "test_util.h"

#include <set>
#include <string>
#include <variant>
#include <vector>

using namespace tgcurl;

namespace {

const CommandSpec* by_tool(const std::string& tool) {
    for (const CommandSpec& spec : registry()) {
        if (spec.tool == tool) {
            return &spec;
        }
    }
    return nullptr;
}

std::string joined_set(const std::set<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out += " ";
        }
        out += item;
    }
    return out;
}

std::string joined(const std::variant<Args, Error>& r) {
    if (!std::holds_alternative<Args>(r)) {
        return "<error: " + std::get<Error>(r).hint() + ">";
    }
    std::string out;
    for (const std::string& a : std::get<Args>(r)) {
        if (!out.empty()) {
            out += " ";
        }
        out += a;
    }
    return out;
}

} // namespace

int main() {
    // --- Registry invariants --------------------------------------------------
    {
        CHECK(!registry().empty());
        std::set<std::string> tools;
        std::set<std::string> cli;
        for (const CommandSpec& spec : registry()) {
            CHECK(!spec.command.empty());
            CHECK(!spec.description.empty());
            CHECK(spec.handler != nullptr);
            // MCP tool names must be unique (empty = CLI-only, may repeat).
            if (!spec.tool.empty()) {
                CHECK(tools.insert(spec.tool).second);
            }
            // command+subcommand pairs must be unique.
            CHECK(cli.insert(spec.command + " " + spec.subcommand).second);
            // A required flag param or a required-after-optional positional
            // would make the CLI mapping ambiguous; forbid both by
            // construction.
            bool saw_optional_positional = false;
            for (const ParamSpec& p : spec.params) {
                CHECK(!p.name.empty());
                CHECK(!p.description.empty());
                if (p.flag.empty()) {
                    CHECK(!(saw_optional_positional && p.required));
                    saw_optional_positional = saw_optional_positional || !p.required;
                }
            }
        }
        // The full surface, as an EXACT set: this fails when a tool is added
        // or removed, forcing the list (and the docs it mirrors) to be
        // updated — a presence-only spot check would stay green forever.
        const std::set<std::string> want_tools = {
            "contacts_list", "contacts_new",  "contacts_block", "chats_list",
            "chat_history",  "search_messages", "send_message", "send_file",
            "send_photo",    "send_gif",      "send_location",  "send_poll",
            "send_checklist", "mark_read"};
        CHECK_EQ(joined_set(tools), joined_set(want_tools));
        // The session-lifecycle commands are CLI-only (empty tool name) — and
        // nothing else is: a new command is agent-facing by default.
        std::set<std::string> cli_only;
        for (const CommandSpec& spec : registry()) {
            if (spec.tool.empty()) {
                CHECK(spec.subcommand.empty());
                cli_only.insert(spec.command);
            }
        }
        CHECK_EQ(joined_set(cli_only), joined_set({"login", "logout", "status"}));
    }

    // --- inputSchema generation -------------------------------------------
    {
        const CommandSpec* send = by_tool("send_message");
        CHECK(send != nullptr);
        std::string schema = input_schema_json(*send);
        CHECK(schema.find("\"type\":\"object\"") != std::string::npos);
        CHECK(schema.find("\"id\":{\"type\":\"string\"") != std::string::npos);
        CHECK(schema.find("\"required\":[\"id\",\"text\"]") != std::string::npos);
    }
    {
        const CommandSpec* chats = by_tool("chats_list");
        CHECK(chats != nullptr);
        std::string schema = input_schema_json(*chats);
        CHECK(schema.find("\"limit\":{\"type\":\"integer\"") != std::string::npos);
        CHECK(schema.find("\"required\":[]") != std::string::npos);
    }

    // --- Named-argument -> CLI-argv mapping --------------------------------
    {
        // Positionals in declaration order regardless of caller order.
        const CommandSpec* send = by_tool("send_message");
        auto r = build_cli_args(*send, {{"text", "hi"}, {"id", "@user"}});
        CHECK_EQ(joined(r), "@user hi");
        // Optional reply flag.
        auto r2 = build_cli_args(*send,
                                 {{"id", "@user"}, {"text", "hi"}, {"reply_to_message_id", "99"}});
        CHECK_EQ(joined(r2), "@user hi --reply-to 99");
    }
    {
        // Subcommand word precedes positionals; trailing optional may be
        // omitted.
        const CommandSpec* cnew = by_tool("contacts_new");
        auto r = build_cli_args(*cnew, {{"phone", "+31"}, {"first_name", "Ada"}});
        CHECK_EQ(joined(r), "new +31 Ada");
        auto r2 = build_cli_args(
            *cnew, {{"phone", "+31"}, {"first_name", "Ada"}, {"last_name", "Lovelace"}});
        CHECK_EQ(joined(r2), "new +31 Ada Lovelace");
    }
    {
        // Optional flag params become "--flag value" and may be omitted.
        const CommandSpec* hist = by_tool("chat_history");
        auto r = build_cli_args(*hist, {{"id", "-100123"}, {"last", "5"}});
        CHECK_EQ(joined(r), "-100123 --last 5");
        auto r2 = build_cli_args(*hist, {{"id", "-100123"}});
        CHECK_EQ(joined(r2), "-100123");
        // Pagination cursor and the service-message switch.
        auto r3 = build_cli_args(*hist, {{"id", "-100123"}, {"before", "777"}, {"all", "true"}});
        CHECK_EQ(joined(r3), "-100123 --before 777 --all");
        auto r4 = build_cli_args(*hist, {{"id", "-100123"}, {"all", "false"}});
        CHECK_EQ(joined(r4), "-100123");
    }
    {
        const CommandSpec* chats = by_tool("chats_list");
        auto r = build_cli_args(*chats, {});
        CHECK_EQ(joined(r), "list");
        auto r2 = build_cli_args(*chats, {{"limit", "7"}});
        CHECK_EQ(joined(r2), "list --limit 7");
        // Boolean flag: true -> bare flag, false -> nothing, junk -> error.
        auto r3 = build_cli_args(*chats, {{"unread", "true"}, {"limit", "7"}});
        CHECK_EQ(joined(r3), "list --limit 7 --unread");
        auto r4 = build_cli_args(*chats, {{"unread", "false"}});
        CHECK_EQ(joined(r4), "list");
        auto r5 = build_cli_args(*chats, {{"unread", "yes"}});
        CHECK(std::holds_alternative<Error>(r5));
        // And the schema calls it a boolean.
        CHECK(input_schema_json(*chats).find("\"unread\":{\"type\":\"boolean\"") !=
              std::string::npos);
    }
    {
        // sendfile: trailing optional caption.
        const CommandSpec* sendfile = by_tool("send_file");
        auto r = build_cli_args(*sendfile, {{"id", "42"}, {"path", "/tmp/r.pdf"}});
        CHECK_EQ(joined(r), "42 /tmp/r.pdf");
        auto r2 = build_cli_args(*sendfile,
                                 {{"caption", "report"}, {"id", "42"}, {"path", "/tmp/r.pdf"}});
        CHECK_EQ(joined(r2), "42 /tmp/r.pdf report");
    }
    {
        // sendphoto: same shape as sendfile — trailing optional caption.
        const CommandSpec* sendphoto = by_tool("send_photo");
        auto r = build_cli_args(*sendphoto, {{"id", "42"}, {"path", "/tmp/p.jpg"}});
        CHECK_EQ(joined(r), "42 /tmp/p.jpg");
        auto r2 = build_cli_args(*sendphoto,
                                 {{"caption", "sunset"}, {"id", "42"}, {"path", "/tmp/p.jpg"}});
        CHECK_EQ(joined(r2), "42 /tmp/p.jpg sunset");
    }
    {
        // Shared send modifiers on every send_* tool: --silent (bare flag),
        // --at, --reply-to.
        const CommandSpec* send = by_tool("send_message");
        auto r = build_cli_args(
            *send, {{"id", "42"}, {"text", "hi"}, {"silent", "true"}, {"schedule_at", "1780000000"}});
        CHECK_EQ(joined(r), "42 hi --silent --at 1780000000");
        const CommandSpec* sendgif = by_tool("send_gif");
        auto r2 = build_cli_args(*sendgif,
                                 {{"id", "42"}, {"path", "/tmp/a.gif"}, {"silent", "true"}});
        CHECK_EQ(joined(r2), "42 /tmp/a.gif --silent");
    }
    {
        // sendlocation: number params map onto positionals verbatim, and the
        // schema calls them numbers.
        const CommandSpec* loc = by_tool("send_location");
        auto r = build_cli_args(*loc,
                                {{"id", "42"}, {"latitude", "52.37"}, {"longitude", "4.89"}});
        CHECK_EQ(joined(r), "42 52.37 4.89");
        CHECK(input_schema_json(*loc).find("\"latitude\":{\"type\":\"number\"") !=
              std::string::npos);
    }
    {
        // sendpoll / sendchecklist: the '|'-list rides as one positional.
        const CommandSpec* poll = by_tool("send_poll");
        auto r = build_cli_args(*poll,
                                {{"id", "42"}, {"question", "lunch?"}, {"options", "yes|no"}});
        CHECK_EQ(joined(r), "42 lunch? yes|no");
        const CommandSpec* checklist = by_tool("send_checklist");
        auto r2 = build_cli_args(
            *checklist, {{"id", "42"}, {"title", "groceries"}, {"tasks", "milk|bread"}});
        CHECK_EQ(joined(r2), "42 groceries milk|bread");
    }
    {
        // search: positional query + optional flags, incl. the pagination cursor.
        const CommandSpec* search = by_tool("search_messages");
        auto r = build_cli_args(*search, {{"query", "deploy"}});
        CHECK_EQ(joined(r), "deploy");
        auto r2 = build_cli_args(*search, {{"limit", "5"}, {"query", "deploy"}, {"chat_id", "@dev"}});
        CHECK_EQ(joined(r2), "deploy --chat @dev --limit 5");
        auto r3 = build_cli_args(*search, {{"query", "deploy"}, {"offset", "abc123"}});
        CHECK_EQ(joined(r3), "deploy --offset abc123");
    }
    {
        // Paged listings: offset flags for chats and contacts.
        const CommandSpec* chats = by_tool("chats_list");
        auto r = build_cli_args(*chats, {{"limit", "50"}, {"offset", "50"}});
        CHECK_EQ(joined(r), "list --limit 50 --offset 50");
        const CommandSpec* contacts = by_tool("contacts_list");
        auto r2 = build_cli_args(*contacts, {{"limit", "100"}, {"offset", "200"}});
        CHECK_EQ(joined(r2), "list --limit 100 --offset 200");
        auto r3 = build_cli_args(*contacts, {});
        CHECK_EQ(joined(r3), "list");
    }

    // --- Mapping errors ------------------------------------------------------
    {
        const CommandSpec* send = by_tool("send_message");
        auto missing = build_cli_args(*send, {{"id", "@user"}});
        CHECK(std::holds_alternative<Error>(missing));
        CHECK_EQ(std::get<Error>(missing).code(), "invalid_params");

        auto unknown = build_cli_args(*send, {{"id", "@user"}, {"text", "x"}, {"oops", "y"}});
        CHECK(std::holds_alternative<Error>(unknown));
        CHECK_EQ(std::get<Error>(unknown).code(), "invalid_params");
    }

    RETURN_TEST_RESULT();
}
