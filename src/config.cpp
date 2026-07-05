#include "config.h"

#include "json_in.h"
#include "json_out.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <variant>

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

// Validate a parsed config.json against its fixed shape:
//   { "api_id": <int>, "api_hash": "<string>" }
// Either key order is fine; anything else (unknown keys, wrong types, missing
// fields) is rejected, surfacing config_invalid. On success fills `config`;
// on failure returns a reason string.
std::optional<std::string> parse_config(const std::string& text, Config& config) {
    std::variant<json::Value, std::string> parsed = json::parse(text);
    if (std::holds_alternative<std::string>(parsed)) {
        return std::get<std::string>(parsed);
    }
    const json::Value& root = std::get<json::Value>(parsed);
    if (root.type != json::Value::Type::Object) {
        return "expected object";
    }

    bool have_api_id = false;
    bool have_api_hash = false;
    for (const auto& [key, value] : root.members) {
        if (key == "api_id") {
            std::optional<std::int64_t> id = value.as_int64();
            if (!id.has_value()) {
                return "api_id must be an integer";
            }
            if (*id <= 0 || *id > INT32_MAX) {
                return "api_id out of range";
            }
            config.api_id = static_cast<int>(*id);
            have_api_id = true;
        } else if (key == "api_hash") {
            if (value.type != json::Value::Type::String) {
                return "api_hash must be a string";
            }
            config.api_hash = value.text;
            have_api_hash = true;
        } else {
            return "unknown key: " + key;
        }
    }
    if (!have_api_id) {
        return "missing api_id";
    }
    if (!have_api_hash) {
        return "missing api_hash";
    }
    if (config.api_hash.empty()) {
        return "api_hash is empty";
    }
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
