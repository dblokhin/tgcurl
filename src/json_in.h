// Minimal JSON parser for tgcurl.
//
// json_out.h covers the write side; this is the read side. Two consumers need
// it: config.json loading and the MCP server's JSON-RPC requests. Both parse
// small, machine-written documents, so a compact strict RFC 8259 parser is
// enough — no third-party dependency, matching the write-side decision.
//
// Numbers are kept as their source lexeme (no double round-trip): tgcurl only
// ever needs them as int64 (ids, limits) or to echo them back verbatim
// (JSON-RPC request ids), and a lexeme does both losslessly.
#ifndef TGCURL_JSON_IN_H
#define TGCURL_JSON_IN_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tgcurl::json {

// A parsed JSON value. A plain tagged struct (not a std::variant) keeps the
// recursive type simple and the accessors cheap.
class Value {
  public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;     // Bool
    std::string number;       // Number: the verbatim source lexeme, e.g. "-12" or "1e3"
    std::string text;         // String: the decoded (unescaped, UTF-8) contents
    std::vector<Value> items; // Array elements, in order
    std::vector<std::pair<std::string, Value>> members; // Object members, in order

    // Object member lookup (first match); nullptr if absent or not an Object.
    [[nodiscard]] const Value* get(const std::string& key) const;

    // The value as an int64 if it is a Number with a pure-integer lexeme that
    // fits; nullopt otherwise (floats, exponents, overflow, non-numbers).
    [[nodiscard]] std::optional<std::int64_t> as_int64() const;
};

// Parse `text` as exactly one JSON document (trailing junk is an error).
// Returns the Value, or a human-readable reason string on malformed input.
[[nodiscard]] std::variant<Value, std::string> parse(const std::string& text);

} // namespace tgcurl::json

#endif // TGCURL_JSON_IN_H
