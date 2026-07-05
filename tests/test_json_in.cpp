// Tests for the minimal JSON parser (json_in.h). Pure logic, no I/O.
#include "json_in.h"
#include "json_out.h"
#include "test_util.h"

#include <array>
#include <string>
#include <variant>

using namespace tgcurl;

namespace {

bool ok(const std::variant<json::Value, std::string>& r) {
    return std::holds_alternative<json::Value>(r);
}

const json::Value& val(const std::variant<json::Value, std::string>& r) {
    return std::get<json::Value>(r);
}

} // namespace

// All std::get<> accesses are guarded by ok() checks; an escaping exception
// would just fail the test.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    // --- Scalars --------------------------------------------------------------
    {
        auto r = json::parse("null");
        CHECK(ok(r));
        CHECK(val(r).type == json::Value::Type::Null);
    }
    {
        auto r = json::parse(" true ");
        CHECK(ok(r));
        CHECK(val(r).type == json::Value::Type::Bool);
        CHECK(val(r).boolean);
    }
    {
        auto r = json::parse("false");
        CHECK(ok(r));
        CHECK(!val(r).boolean);
    }
    {
        auto r = json::parse("-1234567890123");
        CHECK(ok(r));
        CHECK(val(r).type == json::Value::Type::Number);
        CHECK_EQ(val(r).number, "-1234567890123");
        CHECK(val(r).as_int64().has_value());
        CHECK(val(r).as_int64().value() == -1234567890123LL);
    }
    {
        // Floats keep their lexeme but don't convert to int64.
        auto r = json::parse("1.5e3");
        CHECK(ok(r));
        CHECK_EQ(val(r).number, "1.5e3");
        CHECK(!val(r).as_int64().has_value());
    }
    {
        auto r = json::parse(R"("hi \"there\"\n")");
        CHECK(ok(r));
        CHECK(val(r).type == json::Value::Type::String);
        CHECK_EQ(val(r).text, "hi \"there\"\n");
    }
    {
        // \u escapes: ASCII, BMP (UTF-8 encoded), and a surrogate pair.
        auto r = json::parse(R"("AЖ😀")");
        CHECK(ok(r));
        CHECK_EQ(val(r).text, "A\xD0\x96\xF0\x9F\x98\x80");
    }

    // --- Structures -----------------------------------------------------------
    {
        auto r = json::parse(R"({"a": 1, "b": [true, "x"], "c": {"d": null}})");
        CHECK(ok(r));
        const json::Value& v = val(r);
        CHECK(v.type == json::Value::Type::Object);
        CHECK(v.get("a") != nullptr);
        CHECK(v.get("a")->as_int64().value_or(0) == 1);
        const json::Value* b = v.get("b");
        CHECK(b != nullptr && b->type == json::Value::Type::Array);
        CHECK(b != nullptr && b->items.size() == 2);
        CHECK(b != nullptr && b->items[1].text == "x");
        const json::Value* c = v.get("c");
        CHECK(c != nullptr && c->get("d") != nullptr);
        CHECK(c != nullptr && c->get("d")->type == json::Value::Type::Null);
        CHECK(v.get("missing") == nullptr);
    }
    {
        auto r = json::parse("[]");
        CHECK(ok(r));
        CHECK(val(r).items.empty());
    }
    {
        auto r = json::parse("{}");
        CHECK(ok(r));
        CHECK(val(r).members.empty());
    }

    // --- json_out round-trip: what the writer emits, the parser reads --------
    {
        json::Writer w;
        w.field("s", std::string("tab\tquote\"ctl\x01"));
        w.field("n", static_cast<std::int64_t>(-42));
        w.field("b", true);
        auto r = json::parse(w.object());
        CHECK(ok(r));
        CHECK_EQ(val(r).get("s")->text, "tab\tquote\"ctl\x01");
        CHECK(val(r).get("n")->as_int64().value_or(0) == -42);
        CHECK(val(r).get("b")->boolean);
    }

    // --- Malformed inputs are rejected with a reason --------------------------
    const std::array<const char*, 14> bad = {
        "",              // empty
        "nul",           // bad literal
        "{",             // unterminated object
        "[1,",           // unterminated array
        "\"abc",         // unterminated string
        R"({"a" 1})",    // missing colon
        "{a: 1}",        // unquoted key
        "[1 2]",         // missing comma
        "1 2",           // trailing junk
        "-",             // no digits
        "01",            // leading zero
        R"("\x")",       // bad escape
        R"("\ud800")",   // unpaired surrogate
        "\"\x01\"",      // raw control char in string
    };
    for (const char* text : bad) {
        auto r = json::parse(text);
        CHECK(!ok(r));
    }

    // Deep nesting is bounded, not a stack overflow.
    {
        std::string deep(1000, '[');
        deep += std::string(1000, ']');
        auto r = json::parse(deep);
        CHECK(!ok(r));
    }

    RETURN_TEST_RESULT();
}
