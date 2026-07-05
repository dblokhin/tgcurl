#include "json_in.h"

#include <cctype>
#include <exception>
#include <stdexcept>

namespace tgcurl::json {

namespace {

// Nesting cap so a hostile/degenerate document can't overflow the C++ stack
// through recursive descent. Far deeper than anything tgcurl parses.
constexpr int kMaxDepth = 64;

// Thrown internally to unwind with a reason; parse() converts it to the
// error-string variant so callers never see exceptions.
class ParseError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

int hex_val(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

class Parser {
  public:
    explicit Parser(const std::string& text) : s_(&text) {}

    Value parse_document() {
        Value v = parse_value(0);
        skip_ws();
        if (!eof()) {
            throw ParseError("trailing data after document");
        }
        return v;
    }

  private:
    [[nodiscard]] bool eof() const { return i_ >= s_->size(); }
    [[nodiscard]] char peek() const { return (*s_)[i_]; }
    char take() { return (*s_)[i_++]; }

    void skip_ws() {
        while (!eof()) {
            char ch = peek();
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                break;
            }
            ++i_;
        }
    }

    void expect_literal(const std::string& rest) {
        // First char already consumed by the caller; match the remainder.
        for (char expected : rest) {
            if (eof() || take() != expected) {
                throw ParseError("invalid literal");
            }
        }
    }

    Value parse_value(int depth) {
        if (depth > kMaxDepth) {
            throw ParseError("nesting too deep");
        }
        skip_ws();
        if (eof()) {
            throw ParseError("unexpected end of input");
        }
        Value v;
        char ch = peek();
        switch (ch) {
        case '{':
            parse_object(v, depth);
            return v;
        case '[':
            parse_array(v, depth);
            return v;
        case '"':
            v.type = Value::Type::String;
            v.text = parse_string();
            return v;
        case 't':
            ++i_;
            expect_literal("rue");
            v.type = Value::Type::Bool;
            v.boolean = true;
            return v;
        case 'f':
            ++i_;
            expect_literal("alse");
            v.type = Value::Type::Bool;
            v.boolean = false;
            return v;
        case 'n':
            ++i_;
            expect_literal("ull");
            v.type = Value::Type::Null;
            return v;
        default:
            v.type = Value::Type::Number;
            v.number = parse_number();
            return v;
        }
    }

    void parse_object(Value& v, int depth) {
        v.type = Value::Type::Object;
        ++i_; // '{'
        skip_ws();
        if (!eof() && peek() == '}') {
            ++i_;
            return;
        }
        while (true) {
            skip_ws();
            if (eof() || peek() != '"') {
                throw ParseError("expected string key");
            }
            std::string key = parse_string();
            skip_ws();
            if (eof() || take() != ':') {
                throw ParseError("expected ':' after key");
            }
            v.members.emplace_back(std::move(key), parse_value(depth + 1));
            skip_ws();
            if (eof()) {
                throw ParseError("unterminated object");
            }
            char sep = take();
            if (sep == '}') {
                return;
            }
            if (sep != ',') {
                throw ParseError("expected ',' or '}' in object");
            }
        }
    }

    void parse_array(Value& v, int depth) {
        v.type = Value::Type::Array;
        ++i_; // '['
        skip_ws();
        if (!eof() && peek() == ']') {
            ++i_;
            return;
        }
        while (true) {
            v.items.push_back(parse_value(depth + 1));
            skip_ws();
            if (eof()) {
                throw ParseError("unterminated array");
            }
            char sep = take();
            if (sep == ']') {
                return;
            }
            if (sep != ',') {
                throw ParseError("expected ',' or ']' in array");
            }
        }
    }

    // Decode one \uXXXX unit (cursor just past the 'u'). Returns the code unit.
    unsigned parse_hex4() {
        if (i_ + 4 > s_->size()) {
            throw ParseError("truncated \\u escape");
        }
        unsigned code = 0;
        for (int k = 0; k < 4; ++k) {
            int digit = hex_val(take());
            if (digit < 0) {
                throw ParseError("bad \\u escape");
            }
            code = (code << 4U) | static_cast<unsigned>(digit);
        }
        return code;
    }

    // Append a Unicode code point as UTF-8.
    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3FU)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3FU)));
        }
    }

    std::string parse_string() {
        ++i_; // opening quote
        std::string out;
        while (true) {
            if (eof()) {
                throw ParseError("unterminated string");
            }
            char ch = take();
            if (ch == '"') {
                return out;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                throw ParseError("unescaped control character in string");
            }
            if (ch == '\\') {
                parse_escape(out);
            } else {
                out.push_back(ch);
            }
        }
    }

    // Decode one escape sequence (cursor just past the backslash) into `out`.
    void parse_escape(std::string& out) {
        if (eof()) {
            throw ParseError("truncated escape");
        }
        char esc = take();
        switch (esc) {
        case '"':
            out.push_back('"');
            break;
        case '\\':
            out.push_back('\\');
            break;
        case '/':
            out.push_back('/');
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u':
            append_utf8(out, parse_codepoint());
            break;
        default:
            throw ParseError("bad escape character");
        }
    }

    // Decode a \uXXXX unit (cursor just past the 'u'), combining a surrogate
    // pair into its real code point.
    unsigned parse_codepoint() {
        unsigned code = parse_hex4();
        if (code >= 0xD800 && code <= 0xDBFF) {
            // A high surrogate must be paired with a following \uDC00..DFFF.
            if (i_ + 2 > s_->size() || take() != '\\' || take() != 'u') {
                throw ParseError("unpaired surrogate");
            }
            unsigned low = parse_hex4();
            if (low < 0xDC00 || low > 0xDFFF) {
                throw ParseError("unpaired surrogate");
            }
            return 0x10000 + ((code - 0xD800) << 10U) + (low - 0xDC00);
        }
        if (code >= 0xDC00 && code <= 0xDFFF) {
            throw ParseError("unpaired surrogate");
        }
        return code;
    }

    // Consume one-or-more digits; throws if none are present.
    void consume_digits() {
        if (eof() || std::isdigit(static_cast<unsigned char>(peek())) == 0) {
            throw ParseError("invalid number");
        }
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            ++i_;
        }
    }

    std::string parse_number() {
        const std::size_t start = i_;
        if (!eof() && peek() == '-') {
            ++i_;
        }
        // Integer part: a lone 0, or a nonzero digit followed by digits.
        if (eof() || std::isdigit(static_cast<unsigned char>(peek())) == 0) {
            throw ParseError("invalid number");
        }
        if (take() != '0') {
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                ++i_;
            }
        }
        // Optional fraction.
        if (!eof() && peek() == '.') {
            ++i_;
            consume_digits();
        }
        // Optional exponent.
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++i_;
            if (!eof() && (peek() == '+' || peek() == '-')) {
                ++i_;
            }
            consume_digits();
        }
        return s_->substr(start, i_ - start);
    }

    // Pointer (not a reference) so the class stays assignable and to satisfy
    // no-ref-member linting; outlives the short-lived Parser.
    const std::string* s_;
    std::size_t i_ = 0;
};

} // namespace

const Value* Value::get(const std::string& key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto& [k, v] : members) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

std::optional<std::int64_t> Value::as_int64() const {
    if (type != Type::Number) {
        return std::nullopt;
    }
    // Only a pure-integer lexeme qualifies; "1.0" / "1e3" are rejected rather
    // than silently truncated.
    if (number.find('.') != std::string::npos || number.find('e') != std::string::npos ||
        number.find('E') != std::string::npos) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        long long value = std::stoll(number, &consumed, 10);
        if (consumed != number.size()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value);
    } catch (const std::exception&) {
        return std::nullopt; // overflow
    }
}

std::variant<Value, std::string> parse(const std::string& text) {
    try {
        Parser p(text);
        return p.parse_document();
    } catch (const ParseError& e) {
        return std::string(e.what());
    }
}

} // namespace tgcurl::json
