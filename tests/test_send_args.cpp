// Tests for the shared send-argument parsing: the trailing flag parser and
// list splitter (send_common.h) and the media-command argument shape
// (media_args.h). Pure logic — no session, no network; the one filesystem
// touch (media path validation) uses a temp file created here.
#include "commands/media_args.h"
#include "commands/send_common.h"
#include "test_util.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <variant>
#include <vector>

using namespace tgcurl;

namespace {

// parse_send_flags for an expected-success case; CHECKs failure inline.
SendFlags flags_ok(const Args& args, std::size_t start) {
    auto r = parse_send_flags(args, start);
    CHECK(std::holds_alternative<SendFlags>(r));
    if (!std::holds_alternative<SendFlags>(r)) {
        return SendFlags{};
    }
    return std::get<SendFlags>(r);
}

std::string flags_err(const Args& args, std::size_t start) {
    auto r = parse_send_flags(args, start);
    CHECK(std::holds_alternative<Error>(r));
    return std::holds_alternative<Error>(r) ? std::get<Error>(r).code() : std::string();
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out += ",";
        }
        out += item;
    }
    return out;
}

} // namespace

int main() {
    // --- parse_send_flags ---------------------------------------------------
    {
        // No flags at all; start past the end is fine.
        SendFlags f = flags_ok({"42", "hi"}, 2);
        CHECK(!f.silent);
        CHECK(f.reply_to == 0);
        CHECK(f.send_at == 0);
    }
    {
        // All three flags, any order.
        SendFlags f = flags_ok({"--at", "1780000000", "--silent", "--reply-to", "7"}, 0);
        CHECK(f.silent);
        CHECK(f.reply_to == 7);
        CHECK(f.send_at == 1780000000);
    }
    {
        // --at boundary: int32 max is the last representable unix time.
        const std::string max32 = std::to_string(std::numeric_limits<std::int32_t>::max());
        CHECK(flags_ok({"--at", max32}, 0).send_at == std::numeric_limits<std::int32_t>::max());
        const std::string past32 =
            std::to_string(static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1);
        CHECK_EQ(flags_err({"--at", past32}, 0), "usage");
    }
    // Malformed values and unknown options are usage errors.
    CHECK_EQ(flags_err({"--at", "abc"}, 0), "usage");
    CHECK_EQ(flags_err({"--at", "-5"}, 0), "usage");
    CHECK_EQ(flags_err({"--at"}, 0), "usage");            // missing value
    CHECK_EQ(flags_err({"--reply-to", "12abc"}, 0), "usage");
    CHECK_EQ(flags_err({"--reply-to", "0"}, 0), "usage"); // ids are positive
    CHECK_EQ(flags_err({"--reply-to"}, 0), "usage");
    CHECK_EQ(flags_err({"--frobnicate"}, 0), "usage");
    CHECK_EQ(flags_err({"trailing-positional"}, 0), "usage");

    // --- split_list -----------------------------------------------------------
    CHECK_EQ(joined(split_list("a|b|c")), "a,b,c");
    CHECK_EQ(joined(split_list(" a | b ")), "a,b"); // items are trimmed
    CHECK_EQ(joined(split_list("a||b|")), "a,b");   // empty items are dropped
    CHECK_EQ(joined(split_list("|")), "");
    CHECK_EQ(joined(split_list("")), "");
    CHECK_EQ(joined(split_list("single")), "single");

    // --- parse_media_args -----------------------------------------------------
    // A real file, so the offline path validation can pass.
    const std::string tmp = "test_send_args_media.bin";
    std::ofstream(tmp) << "x";
    {
        // Caption present, then flags.
        auto r = parse_media_args({"42", tmp, "hello", "--silent"}, "usage-text");
        CHECK(std::holds_alternative<MediaArgs>(r));
        const auto& ma = std::get<MediaArgs>(r);
        CHECK_EQ(ma.id_arg, "42");
        CHECK_EQ(ma.caption, "hello");
        CHECK(ma.flags.silent);
        CHECK(!ma.abs_path.empty() && ma.abs_path.front() == '/'); // absolutized
    }
    {
        // No caption: a "--…" third token is a flag, not a caption.
        auto r = parse_media_args({"42", tmp, "--silent"}, "usage-text");
        CHECK(std::holds_alternative<MediaArgs>(r));
        const auto& ma = std::get<MediaArgs>(r);
        CHECK_EQ(ma.caption, "");
        CHECK(ma.flags.silent);
    }
    {
        // Missing file -> file_not_found (after arg parsing, before any session).
        auto r = parse_media_args({"42", "/definitely/not/a/file.bin"}, "usage-text");
        CHECK(std::holds_alternative<Error>(r));
        CHECK_EQ(std::get<Error>(r).code(), "file_not_found");
    }
    {
        // Too few args -> the command's own usage text.
        auto r = parse_media_args({"42"}, "usage-text");
        CHECK(std::holds_alternative<Error>(r));
        CHECK_EQ(std::get<Error>(r).hint(), "usage-text");
    }
    std::remove(tmp.c_str());

    RETURN_TEST_RESULT();
}
