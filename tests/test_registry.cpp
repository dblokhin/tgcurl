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
        // The full surface is registered: the session-lifecycle commands are
        // CLI-only, everything else is also an MCP tool.
        for (const char* cli_only : {"login", "logout", "status"}) {
            CHECK(cli.count(std::string(cli_only) + " ") == 1);
            CHECK(by_tool(cli_only) == nullptr);
        }
        for (const char* tool :
             {"contacts_list", "contacts_new", "contacts_block", "chats_list", "chat_history",
              "send_message", "search_messages", "send_file", "mark_read"}) {
            CHECK(by_tool(tool) != nullptr);
        }
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
        // search: positional query + two optional flags.
        const CommandSpec* search = by_tool("search_messages");
        auto r = build_cli_args(*search, {{"query", "deploy"}});
        CHECK_EQ(joined(r), "deploy");
        auto r2 = build_cli_args(*search, {{"limit", "5"}, {"query", "deploy"}, {"chat_id", "@dev"}});
        CHECK_EQ(joined(r2), "deploy --chat @dev --limit 5");
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
