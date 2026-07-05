#include "mcp.h"

#include "commands/registry.h"
#include "error.h"
#include "json_in.h"
#include "json_out.h"
#include "tgcurl_version.h"

#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tgcurl::mcp {

namespace {

// JSON-RPC 2.0 error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

// The MCP protocol revision this server implements. tgcurl only uses the
// version-stable core (initialize / tools), so if the client proposes a
// different revision we echo it back rather than force a downgrade dance.
constexpr const char* kProtocolVersion = "2024-11-05";

// The request id, re-serialized for the response. JSON-RPC allows a number,
// a string, or null; a missing id means the message is a notification (no
// response at all) — signalled here by nullopt.
std::optional<std::string> id_json(const json::Value& request) {
    const json::Value* id = request.get("id");
    if (id == nullptr) {
        return std::nullopt;
    }
    switch (id->type) {
    case json::Value::Type::Number:
        return id->number; // verbatim lexeme — no reformatting round-trip
    case json::Value::Type::String:
        return json::quote(id->text);
    default:
        return std::string("null");
    }
}

// One response line. MCP's stdio transport is newline-delimited, so the
// (single-line) JSON is terminated by '\n' and flushed immediately — the
// client blocks on it.
void write_message(std::ostream& out, const std::string& body) {
    out << body << '\n';
    out.flush();
}

void write_result(std::ostream& out, const std::string& id, const std::string& result) {
    json::Writer w;
    w.raw_field("jsonrpc", json::quote("2.0"));
    w.raw_field("id", id);
    w.raw_field("result", result);
    write_message(out, w.object());
}

void write_error(std::ostream& out, const std::string& id, int code, const std::string& message) {
    json::Writer err;
    err.field("code", code);
    err.field("message", message);
    json::Writer w;
    w.raw_field("jsonrpc", json::quote("2.0"));
    w.raw_field("id", id);
    w.raw_field("error", err.object());
    write_message(out, w.object());
}

std::string initialize_result(const json::Value& request) {
    // Echo the client's proposed protocol revision when present (see
    // kProtocolVersion above).
    std::string protocol = kProtocolVersion;
    const json::Value* params = request.get("params");
    if (params != nullptr) {
        const json::Value* pv = params->get("protocolVersion");
        if (pv != nullptr && pv->type == json::Value::Type::String && !pv->text.empty()) {
            protocol = pv->text;
        }
    }

    json::Writer tools; // no optional tool features (listChanged etc.)
    json::Writer capabilities;
    capabilities.raw_field("tools", tools.object());

    json::Writer server_info;
    server_info.field("name", "tgcurl");
    server_info.field("version", version());

    json::Writer w;
    w.field("protocolVersion", protocol);
    w.raw_field("capabilities", capabilities.object());
    w.raw_field("serverInfo", server_info.object());
    return w.object();
}

std::string tools_list_result() {
    json::ArrayWriter tools;
    for (const CommandSpec& spec : registry()) {
        if (spec.tool.empty()) {
            continue; // CLI-only (login)
        }
        json::Writer tool;
        tool.field("name", spec.tool);
        tool.field("description", spec.description);
        tool.raw_field("inputSchema", input_schema_json(spec));
        tools.element(tool.object());
    }
    json::Writer w;
    w.raw_field("tools", tools.array());
    return w.object();
}

// Stringify one MCP argument value for the CLI arg vector. The CLI layer
// re-validates (e.g. --limit must parse as a positive integer), so this only
// rejects shapes that have no CLI spelling at all.
std::optional<std::string> stringify_argument(const json::Value& value) {
    switch (value.type) {
    case json::Value::Type::String:
        return value.text;
    case json::Value::Type::Number:
        return value.number; // verbatim lexeme; also lets a numeric chat_id
                             // satisfy a string param
    case json::Value::Type::Bool:
        return std::string(value.boolean ? "true" : "false");
    default:
        return std::nullopt; // null / array / object
    }
}

// The tools/call result object. Tool-level failures (an Error from the
// handler) are reported inside the result with isError=true — per MCP they
// are tool outcomes for the agent to read, not protocol errors.
std::string tool_result(const std::string& text, bool is_error) {
    json::Writer content;
    content.field("type", "text");
    content.field("text", text);
    json::ArrayWriter arr;
    arr.element(content.object());
    json::Writer w;
    w.raw_field("content", arr.array());
    w.field("isError", is_error);
    return w.object();
}

void handle_tools_call(std::ostream& out, const std::string& id, const json::Value& request) {
    const json::Value* params = request.get("params");
    const json::Value* name = params != nullptr ? params->get("name") : nullptr;
    if (name == nullptr || name->type != json::Value::Type::String) {
        write_error(out, id, kInvalidParams, "missing tool name");
        return;
    }

    const CommandSpec* spec = nullptr;
    for (const CommandSpec& s : registry()) {
        if (!s.tool.empty() && s.tool == name->text) {
            spec = &s;
            break;
        }
    }
    if (spec == nullptr) {
        write_error(out, id, kInvalidParams, "unknown tool: " + name->text);
        return;
    }

    // Collect the named arguments as strings.
    std::vector<std::pair<std::string, std::string>> named;
    const json::Value* arguments = params->get("arguments");
    if (arguments != nullptr && arguments->type == json::Value::Type::Object) {
        for (const auto& [arg_name, value] : arguments->members) {
            std::optional<std::string> text = stringify_argument(value);
            if (!text.has_value()) {
                write_error(out, id, kInvalidParams,
                            "argument " + arg_name + " must be a string, number or boolean");
                return;
            }
            named.emplace_back(arg_name, std::move(*text));
        }
    }

    std::variant<Args, Error> cli_args = build_cli_args(*spec, named);
    if (std::holds_alternative<Error>(cli_args)) {
        write_error(out, id, kInvalidParams, std::get<Error>(cli_args).hint());
        return;
    }

    // Run the exact CLI handler, capturing what it would have printed.
    std::ostringstream captured;
    std::optional<Error> err = spec->handler(std::get<Args>(cli_args), captured);
    if (err.has_value()) {
        write_result(out, id, tool_result(err->to_json(), /*is_error=*/true));
        return;
    }
    std::string text = captured.str();
    if (!text.empty() && text.back() == '\n') {
        text.pop_back(); // json::emit's trailing newline; the frame adds its own
    }
    write_result(out, id, tool_result(text, /*is_error=*/false));
}

// Dispatch one parsed JSON-RPC message.
void handle_message(std::ostream& out, const json::Value& request) {
    if (request.type != json::Value::Type::Object) {
        write_error(out, "null", kInvalidRequest, "expected a JSON-RPC object");
        return;
    }
    const json::Value* method = request.get("method");
    std::optional<std::string> id = id_json(request);
    if (method == nullptr || method->type != json::Value::Type::String) {
        // A message with an id but no method is a malformed request; anything
        // else (e.g. a stray response) is silently ignored.
        if (id.has_value()) {
            write_error(out, *id, kInvalidRequest, "missing method");
        }
        return;
    }

    if (!id.has_value()) {
        // Notification: nothing to answer. notifications/initialized and
        // friends need no action from a stateless server.
        return;
    }

    if (method->text == "initialize") {
        write_result(out, *id, initialize_result(request));
    } else if (method->text == "ping") {
        write_result(out, *id, "{}");
    } else if (method->text == "tools/list") {
        write_result(out, *id, tools_list_result());
    } else if (method->text == "tools/call") {
        handle_tools_call(out, *id, request);
    } else {
        write_error(out, *id, kMethodNotFound, "method not found: " + method->text);
    }
}

} // namespace

int serve(std::istream& in, std::ostream& out) {
    // Startup notice goes to STDERR: stdout belongs to the protocol, and a
    // human running `tgcurl -mcp` by hand would otherwise stare at a silent
    // terminal wondering whether anything started.
    std::cerr << "tgcurl " << version()
              << ": MCP server ready (stdio transport, JSON-RPC per line); "
                 "waiting for an MCP client on stdin. Ctrl+C or EOF stops it.\n";

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::variant<json::Value, std::string> parsed = json::parse(line);
        if (std::holds_alternative<std::string>(parsed)) {
            write_error(out, "null", kParseError, "parse error: " + std::get<std::string>(parsed));
            continue;
        }
        handle_message(out, std::get<json::Value>(parsed));
    }
    std::cerr << "tgcurl: MCP client disconnected; shutting down.\n";
    return 0; // EOF: client closed the pipe — clean shutdown
}

} // namespace tgcurl::mcp
