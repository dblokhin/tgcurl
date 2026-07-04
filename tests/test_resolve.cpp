// Tests for the pure part of identifier resolution: classify(). The
// searchPublicChat round-trip in resolve_id() is network-dependent and is
// exercised manually (issue #9).
#include "resolve.h"
#include "test_util.h"

#include <string>

using namespace tgcurl;

namespace {
std::string kind_name(IdKind k) {
    switch (k) {
    case IdKind::ChatId:
        return "chat_id";
    case IdKind::Username:
        return "username";
    case IdKind::Unresolvable:
        return "unresolvable";
    }
    return "?";
}
} // namespace

int main() {
    // Purely numeric → chat_id.
    {
        Classified c = classify("123456789");
        CHECK_EQ(kind_name(c.kind), "chat_id");
        CHECK_EQ(std::to_string(c.chat_id), "123456789");
    }
    // Negative numeric (groups/channels have negative chat_ids) → chat_id.
    {
        Classified c = classify("-1001234567890");
        CHECK_EQ(kind_name(c.kind), "chat_id");
        CHECK_EQ(std::to_string(c.chat_id), "-1001234567890");
    }
    // @username → username, '@' stripped.
    {
        Classified c = classify("@durov");
        CHECK_EQ(kind_name(c.kind), "username");
        CHECK_EQ(c.username, "durov");
    }
    // A bare '@' is not a valid username.
    CHECK_EQ(kind_name(classify("@").kind), "unresolvable");
    // A lone '-' is not a number.
    CHECK_EQ(kind_name(classify("-").kind), "unresolvable");
    // Free text (a display name) is never resolved — no fuzzy matching.
    CHECK_EQ(kind_name(classify("John Smith").kind), "unresolvable");
    CHECK_EQ(kind_name(classify("john").kind), "unresolvable");
    // Mixed alnum is not numeric.
    CHECK_EQ(kind_name(classify("123abc").kind), "unresolvable");
    // Empty is unresolvable.
    CHECK_EQ(kind_name(classify("").kind), "unresolvable");
    // A number with surrounding junk is not a clean chat_id.
    CHECK_EQ(kind_name(classify("12 34").kind), "unresolvable");
    // Overflowing int64 falls through to unresolvable (not a valid chat_id).
    CHECK_EQ(kind_name(classify("99999999999999999999999").kind), "unresolvable");

    RETURN_TEST_RESULT();
}
