#include "config.h"

#include "json_out.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace tgcurl {

namespace {

// A malformed-config error carrying the underlying reason.
Error invalid(const std::string& why) {
    return {"config_invalid", "config.json is malformed: " + why};
}

// Read an environment variable, treating an empty value as unset.
const char* env_nonempty(const char* name) {
    const char* value = std::getenv(name);
    if (value != nullptr && *value != '\0') {
        return value;
    }
    return nullptr;
}

// --- Minimal JSON reader for the fixed config shape -------------------------
// config.json is written by tgcurl itself and holds exactly two fields:
//   { "api_id": <int>, "api_hash": "<string>" }
// Rather than take a JSON dependency (json_out.h is write-only), we hand-parse
// this narrow, known shape. It tolerates surrounding whitespace and either key
// order but rejects anything it doesn't understand, surfacing config_invalid.

// A tiny cursor over the input string (holds a pointer so it stays copyable
// and assignable, and to satisfy no-ref-member linting).
struct Cursor {
    const std::string* s;
    std::size_t i = 0;

    explicit Cursor(const std::string& str) : s(&str) {}

    void skip_ws() {
        while (i < s->size() && (std::isspace(static_cast<unsigned char>((*s)[i])) != 0)) {
            ++i;
        }
    }
    [[nodiscard]] bool eof() const { return i >= s->size(); }
    [[nodiscard]] char peek() const { return (*s)[i]; }
    char take() { return (*s)[i++]; }
};

// Decode a hex digit; returns -1 if not a hex digit.
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

// Handle the character after a backslash, appending the decoded byte to `out`.
// The cursor is positioned just past the backslash on entry.
bool parse_escape(Cursor& c, std::string& out) {
    if (c.eof()) {
        return false;
    }
    switch (c.take()) {
    case '"':
        out.push_back('"');
        return true;
    case '\\':
        out.push_back('\\');
        return true;
    case '/':
        out.push_back('/');
        return true;
    case 'b':
        out.push_back('\b');
        return true;
    case 'f':
        out.push_back('\f');
        return true;
    case 'n':
        out.push_back('\n');
        return true;
    case 'r':
        out.push_back('\r');
        return true;
    case 't':
        out.push_back('\t');
        return true;
    case 'u': {
        // \uXXXX — accept the ASCII range json_out.h emits (control chars
        // < 0x20); higher code points aren't produced by us, so refuse them
        // rather than mangle multi-byte output.
        if (c.i + 4 > c.s->size()) {
            return false;
        }
        int code = 0;
        for (int k = 0; k < 4; ++k) {
            int digit = hex_val(c.take());
            if (digit < 0) {
                return false;
            }
            code = (code << 4) | digit;
        }
        if (code > 0x7F) {
            return false;
        }
        out.push_back(static_cast<char>(code));
        return true;
    }
    default:
        return false;
    }
}

// Parse a JSON string literal at the cursor into `out`. On entry the cursor
// must be at the opening quote. Handles the escapes json_out.h can emit.
bool parse_string(Cursor& c, std::string& out) {
    if (c.eof() || c.peek() != '"') {
        return false;
    }
    ++c.i; // opening quote
    out.clear();
    while (!c.eof()) {
        char ch = c.take();
        if (ch == '"') {
            return true;
        }
        if (ch == '\\') {
            if (!parse_escape(c, out)) {
                return false;
            }
        } else {
            out.push_back(ch);
        }
    }
    return false; // unterminated string
}

// Parse a JSON integer at the cursor into `out`.
bool parse_int(Cursor& c, std::int64_t& out) {
    c.skip_ws();
    std::size_t start = c.i;
    if (!c.eof() && (c.peek() == '-' || c.peek() == '+')) {
        ++c.i;
    }
    std::size_t digits_start = c.i;
    while (!c.eof() && (std::isdigit(static_cast<unsigned char>(c.peek())) != 0)) {
        ++c.i;
    }
    if (c.i == digits_start) {
        return false; // no digits
    }
    // Reject fractional/exponent forms — api_id is an integer.
    if (!c.eof() && (c.peek() == '.' || c.peek() == 'e' || c.peek() == 'E')) {
        return false;
    }
    const std::string tok = c.s->substr(start, c.i - start);
    try {
        std::size_t consumed = 0;
        long long value = std::stoll(tok, &consumed, 10);
        if (consumed != tok.size()) {
            return false; // trailing junk in the token
        }
        out = static_cast<std::int64_t>(value);
        return true;
    } catch (const std::exception&) {
        return false; // out of range or not a number
    }
}

// Accumulator for the fields seen while parsing the config object.
struct ConfigFields {
    bool have_api_id = false;
    bool have_api_hash = false;
    std::int64_t api_id = 0;
    std::string api_hash;
};

// Parse one "<key>": <value> pair at the cursor into `fields`. The cursor is
// positioned at the opening quote of the key. Returns a reason string on error.
std::optional<std::string> parse_field(Cursor& c, ConfigFields& fields) {
    std::string key;
    if (!parse_string(c, key)) {
        return "expected string key";
    }
    c.skip_ws();
    if (c.eof() || c.peek() != ':') {
        return "expected ':' after key";
    }
    ++c.i;
    c.skip_ws();

    if (key == "api_id") {
        if (!parse_int(c, fields.api_id)) {
            return "api_id must be an integer";
        }
        if (fields.api_id <= 0 || fields.api_id > INT32_MAX) {
            return "api_id out of range";
        }
        fields.have_api_id = true;
        return std::nullopt;
    }
    if (key == "api_hash") {
        if (!parse_string(c, fields.api_hash)) {
            return "api_hash must be a string";
        }
        fields.have_api_hash = true;
        return std::nullopt;
    }
    return "unknown key: " + key;
}

// Parse the whole config object. On success fills `config`; on failure returns
// a reason string.
std::optional<std::string> parse_config(const std::string& text, Config& config) {
    Cursor c{text};
    c.skip_ws();
    if (c.eof() || c.peek() != '{') {
        return "expected object";
    }
    ++c.i;

    ConfigFields fields;

    c.skip_ws();
    if (!c.eof() && c.peek() == '}') {
        ++c.i;
        return "missing api_id and api_hash";
    }

    while (true) {
        c.skip_ws();
        if (std::optional<std::string> why = parse_field(c, fields)) {
            return why;
        }
        c.skip_ws();
        if (c.eof()) {
            return "unterminated object";
        }
        char sep = c.take();
        if (sep == ',') {
            continue;
        }
        if (sep == '}') {
            break;
        }
        return "expected ',' or '}'";
    }

    c.skip_ws();
    if (!c.eof()) {
        return "trailing data after object";
    }
    if (!fields.have_api_id) {
        return "missing api_id";
    }
    if (!fields.have_api_hash) {
        return "missing api_hash";
    }
    if (fields.api_hash.empty()) {
        return "api_hash is empty";
    }

    config.api_id = static_cast<int>(fields.api_id);
    config.api_hash = fields.api_hash;
    return std::nullopt;
}

} // namespace

std::string config_dir() {
    if (const char* override_dir = env_nonempty("TGCURL_CONFIG_DIR")) {
        return override_dir;
    }
    if (const char* home = env_nonempty("HOME")) {
        return std::string(home) + "/.config/tgcurl";
    }
    // No HOME and no override: fall back to a relative dir rather than crash;
    // callers surface any resulting I/O error as config_invalid.
    return ".config/tgcurl";
}

std::string config_file_path() {
    return config_dir() + "/config.json";
}

std::string database_dir() {
    return config_dir() + "/td.db";
}

std::variant<Config, Error> load_config() {
    const std::string path = config_file_path();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // Absent (or unreadable) — treat as missing so the caller can steer to
        // the not_authorized / "run: tgcurl login" path.
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0) {
            return Error("config_missing", "no config.json; run: tgcurl login");
        }
        return invalid("cannot open for reading");
    }

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return invalid("read error");
    }

    Config config;
    if (std::optional<std::string> why = parse_config(text, config)) {
        return invalid(*why);
    }
    return config;
}

std::optional<Error> save_config(const Config& config) {
    const std::string dir = config_dir();

    // Create the config directory (0700) if absent. mkdir is a no-op-ish EEXIST
    // when it already exists, which we ignore.
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        return Error("config_io", "cannot create config dir " + dir + ": " + std::strerror(errno));
    }
    // Tighten perms even if the directory pre-existed with looser bits.
    if (::chmod(dir.c_str(), 0700) != 0) {
        return Error("config_io", "cannot chmod config dir: " + std::string(std::strerror(errno)));
    }

    // Serialize with the blessed JSON writer.
    json::Writer w;
    w.field("api_id", config.api_id);
    w.field("api_hash", config.api_hash);
    std::string body = w.object();
    body.push_back('\n');

    // Write atomically: write to a temp file, then rename over the target so a
    // crash never leaves a half-written config.json. The temp file is created
    // with mode 0600 up front (via open, not ofstream) so the api_hash secret
    // is never briefly world-readable between write and chmod.
    const std::string path = config_file_path();
    const std::string tmp = path + ".tmp";

    // POSIX open()/write() (not std::ofstream) so the file is created at 0600
    // atomically — no umask-dependent window where the secret is readable.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return Error("config_io", "cannot open " + tmp + " for writing: " + std::strerror(errno));
    }
    std::size_t written = 0;
    while (written < body.size()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        ssize_t n = ::write(fd, body.data() + written, body.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            ::remove(tmp.c_str());
            return Error("config_io", "write failed for " + tmp + ": " + std::strerror(errno));
        }
        written += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) {
        ::remove(tmp.c_str());
        return Error("config_io", "close failed for " + tmp + ": " + std::strerror(errno));
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::remove(tmp.c_str());
        return Error("config_io", "cannot rename into place: " + std::string(std::strerror(errno)));
    }
    return std::nullopt;
}

std::optional<Error> ensure_database_dir() {
    const std::string dir = config_dir();
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        return Error("config_io", "cannot create config dir " + dir + ": " + std::strerror(errno));
    }
    const std::string db = database_dir();
    if (::mkdir(db.c_str(), 0700) != 0 && errno != EEXIST) {
        return Error("config_io", "cannot create db dir " + db + ": " + std::strerror(errno));
    }
    if (::chmod(db.c_str(), 0700) != 0) {
        return Error("config_io", "cannot chmod db dir: " + std::string(std::strerror(errno)));
    }
    return std::nullopt;
}

} // namespace tgcurl
