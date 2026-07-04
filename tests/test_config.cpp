// Tests for config path resolution and config.json load/save. All I/O is
// confined to a scratch dir via TGCURL_CONFIG_DIR, so no real ~/.config is
// touched and nothing hits the network.
#include "config.h"
#include "test_util.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <variant>

using namespace tgcurl;

namespace {

// A unique scratch directory for this test process, set as TGCURL_CONFIG_DIR.
std::string setup_scratch() {
    std::string dir = "/tmp/tgcurl_test_config_" + std::to_string(::getpid());
    // Start clean each run.
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ::setenv("TGCURL_CONFIG_DIR", dir.c_str(), /*overwrite=*/1);
    return dir;
}

// Return the permission bits (the low 12) of a path, or -1 if it doesn't exist.
int perms_of(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return static_cast<int>(st.st_mode & 07777);
}

// Overwrite config.json with raw bytes (to exercise the malformed path).
void write_raw(const std::string& text) {
    std::ofstream out(config_file_path(), std::ios::binary | std::ios::trunc);
    out << text;
}

bool is_config(const std::variant<Config, Error>& v) {
    return std::holds_alternative<Config>(v);
}

} // namespace

// All std::get<> accesses below are guarded by is_config() checks, so they
// never actually throw; an escaping exception here would just fail the test.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    const std::string dir = setup_scratch();

    // --- Path resolution ----------------------------------------------------
    CHECK_EQ(config_dir(), dir);
    CHECK_EQ(config_file_path(), dir + "/config.json");
    CHECK_EQ(database_dir(), dir + "/td.db");

    // TGCURL_CONFIG_DIR takes precedence over HOME.
    ::setenv("HOME", "/nonexistent-home", 1);
    CHECK_EQ(config_dir(), dir);

    // With no override, falls back to $HOME/.config/tgcurl.
    ::unsetenv("TGCURL_CONFIG_DIR");
    ::setenv("HOME", "/home/someone", 1);
    CHECK_EQ(config_dir(), "/home/someone/.config/tgcurl");
    // Restore the scratch override for the rest of the test.
    ::setenv("TGCURL_CONFIG_DIR", dir.c_str(), 1);

    // --- Missing config -----------------------------------------------------
    {
        auto loaded = load_config();
        CHECK(!is_config(loaded));
        CHECK_EQ(std::get<Error>(loaded).code(), "config_missing");
    }

    // --- Round-trip + perms -------------------------------------------------
    {
        Config c;
        c.api_id = 123456;
        c.api_hash = "abcDEF0123456789";
        auto err = save_config(c);
        CHECK(!err.has_value());

        // File 0600, directory 0700.
        CHECK_EQ(std::to_string(perms_of(config_file_path())), std::to_string(0600));
        CHECK_EQ(std::to_string(perms_of(dir)), std::to_string(0700));
        // No stray temp file left behind.
        CHECK_EQ(std::to_string(perms_of(config_file_path() + ".tmp")), std::to_string(-1));

        auto loaded = load_config();
        CHECK(is_config(loaded));
        const Config& got = std::get<Config>(loaded);
        CHECK_EQ(std::to_string(got.api_id), "123456");
        CHECK_EQ(got.api_hash, "abcDEF0123456789");
    }

    // Overwrite (idempotent save) with new values round-trips too.
    {
        Config c;
        c.api_id = 42;
        c.api_hash = R"(hash-with-"quote"-and-\slash)";
        CHECK(!save_config(c).has_value());
        auto loaded = load_config();
        CHECK(is_config(loaded));
        CHECK_EQ(std::to_string(std::get<Config>(loaded).api_id), "42");
        CHECK_EQ(std::get<Config>(loaded).api_hash, R"(hash-with-"quote"-and-\slash)");
    }

    // --- Malformed configs report config_invalid ----------------------------
    auto expect_invalid = [](const std::string& text) {
        write_raw(text);
        auto loaded = load_config();
        CHECK(!is_config(loaded));
        if (!is_config(loaded)) {
            CHECK_EQ(std::get<Error>(loaded).code(), "config_invalid");
        }
    };
    expect_invalid("not json at all");
    expect_invalid("{");
    expect_invalid("{}");                                    // missing both fields
    expect_invalid(R"({"api_id": 1})");                      // missing api_hash
    expect_invalid(R"({"api_hash": "x"})");                  // missing api_id
    expect_invalid(R"({"api_id": "str", "api_hash": "x"})"); // api_id not int
    expect_invalid(R"({"api_id": 1, "api_hash": 2})");       // api_hash not string
    expect_invalid(R"({"api_id": 0, "api_hash": "x"})");     // api_id must be positive
    expect_invalid(R"({"api_id": 1, "api_hash": ""})");      // empty api_hash
    expect_invalid(R"({"api_id": 1, "api_hash": "x", "extra": 3})"); // unknown key
    expect_invalid(R"({"api_id": 1, "api_hash": "x"} trailing)");    // trailing junk

    // A well-formed config with surrounding whitespace is accepted.
    {
        write_raw("  { \"api_hash\" : \"zzz\" , \"api_id\" : 777 }  \n");
        auto loaded = load_config();
        CHECK(is_config(loaded));
        if (is_config(loaded)) {
            CHECK_EQ(std::to_string(std::get<Config>(loaded).api_id), "777");
            CHECK_EQ(std::get<Config>(loaded).api_hash, "zzz");
        }
    }

    // --- ensure_database_dir ------------------------------------------------
    {
        CHECK_EQ(std::to_string(perms_of(database_dir())), std::to_string(-1)); // not created yet
        auto err = ensure_database_dir();
        CHECK(!err.has_value());
        CHECK_EQ(std::to_string(perms_of(database_dir())), std::to_string(0700));
        // Idempotent: a second call succeeds.
        CHECK(!ensure_database_dir().has_value());
    }

    // Cleanup.
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    RETURN_TEST_RESULT();
}
