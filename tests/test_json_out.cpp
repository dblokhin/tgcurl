#include "error.h"
#include "json_out.h"
#include "test_util.h"

using namespace tgcurl;

int main() {
    // --- string escaping ---
    CHECK_EQ(json::quote("hi"), "\"hi\"");
    CHECK_EQ(json::quote("a\"b"), "\"a\\\"b\"");
    CHECK_EQ(json::quote("a\\b"), "\"a\\\\b\"");
    CHECK_EQ(json::quote("line\nbreak"), "\"line\\nbreak\"");
    CHECK_EQ(json::quote(std::string("\x01")), "\"\\u0001\"");
    CHECK_EQ(json::quote("tab\there"), "\"tab\\there\"");

    // --- object writer: types and ordering ---
    {
        json::Writer w;
        w.field("ok", true).field("id", static_cast<std::int64_t>(42)).field("name", "alice");
        CHECK_EQ(w.object(), "{\"ok\":true,\"id\":42,\"name\":\"alice\"}");
    }
    // empty object
    {
        json::Writer w;
        CHECK_EQ(w.object(), "{}");
    }
    // raw_field nests pre-serialized JSON (e.g. an array)
    {
        json::Writer w;
        w.field("ok", true).raw_field("items", "[1,2,3]");
        CHECK_EQ(w.object(), "{\"ok\":true,\"items\":[1,2,3]}");
    }

    // --- error serialization ---
    {
        Error e("not_authorized", "run: tgcurl login");
        CHECK_EQ(e.to_json(), "{\"error\":\"not_authorized\",\"hint\":\"run: tgcurl login\"}");
    }
    {
        Error e("boom", "");
        CHECK_EQ(e.to_json(), "{\"error\":\"boom\"}");
    }

    RETURN_TEST_RESULT();
}
