// Minimal JSON output for tgcurl.
//
// tgcurl only ever *emits* JSON (it never parses arbitrary JSON on the hot
// path), and the shapes are simple: objects, arrays, strings, int64, bool,
// null. So instead of a JSON dependency we use a tiny, correct writer with
// proper RFC 8259 string escaping. Success goes to stdout; errors go to
// stderr as a JSON object, paired with a non-zero exit code.
#ifndef TGCURL_JSON_OUT_H
#define TGCURL_JSON_OUT_H

#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>

namespace tgcurl::json {

// Escape a string into a JSON string literal (including surrounding quotes).
inline std::string quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                // Other control characters must be \u-escaped.
                static const char* hex = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

// A small builder for a single JSON object or array. Values are appended
// pre-serialized; `raw()` accepts already-valid JSON (used to nest arrays).
class Writer {
  public:
    // Build an object: writer.field("ok", true).field("id", 42).object()
    Writer& field(const std::string& key, const std::string& value) {
        return raw_field(key, quote(value));
    }
    Writer& field(const std::string& key, const char* value) {
        return raw_field(key, quote(value));
    }
    Writer& field(const std::string& key, std::int64_t value) {
        return raw_field(key, std::to_string(value));
    }
    Writer& field(const std::string& key, int value) {
        return raw_field(key, std::to_string(value));
    }
    Writer& field(const std::string& key, bool value) {
        return raw_field(key, value ? "true" : "false");
    }
    // Field whose value is already-serialized JSON (array/object/number).
    Writer& raw_field(const std::string& key, const std::string& raw_value) {
        if (!first_) {
            buf_ << ',';
        }
        first_ = false;
        buf_ << quote(key) << ':' << raw_value;
        return *this;
    }

    [[nodiscard]] std::string object() const { return "{" + buf_.str() + "}"; }

  private:
    std::ostringstream buf_;
    bool first_ = true;
};

// Builder for a top-level JSON array. Commands like `contacts list`, `chats
// list` and `chat` emit arrays of objects; this is the one blessed path for
// comma-joining pre-serialized elements so callers don't hand-roll it.
class ArrayWriter {
  public:
    // Append an already-serialized JSON value (typically a Writer::object()).
    ArrayWriter& element(const std::string& raw_value) {
        if (!first_) {
            buf_ << ',';
        }
        first_ = false;
        buf_ << raw_value;
        return *this;
    }

    [[nodiscard]] std::string array() const { return "[" + buf_.str() + "]"; }

  private:
    std::ostringstream buf_;
    bool first_ = true;
};

// Print a serialized JSON value to the given stream (stdout in production),
// with a trailing newline.
inline void emit(const std::string& json_value, std::ostream& os) {
    os << json_value << '\n';
}

} // namespace tgcurl::json

#endif // TGCURL_JSON_OUT_H
