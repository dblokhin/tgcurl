// App credentials and storage-path resolution for tgcurl.
//
// tgcurl keeps two things on disk under a single config directory:
//   - config.json  = { "api_id": int, "api_hash": string } from my.telegram.org
//   - td.db/       = TDLib's encrypted session database (the persisted login)
// This module resolves that directory (XDG, overridable via TGCURL_CONFIG_DIR)
// and loads/saves config.json. It is pure logic — no TDLib — so it lives in
// tgcurl_core and is unit-testable without a network.
#ifndef TGCURL_CONFIG_H
#define TGCURL_CONFIG_H

#include "error.h"

#include <optional>
#include <string>
#include <variant>

namespace tgcurl {

// The long-lived app credentials from my.telegram.org, mirrored in config.json.
struct Config {
    int api_id = 0;
    std::string api_hash;
};

// Absolute path to the config directory: $TGCURL_CONFIG_DIR if set and
// non-empty, else $HOME/.config/tgcurl. No trailing slash.
[[nodiscard]] std::string config_dir();

// Absolute path to config.json inside the config directory.
[[nodiscard]] std::string config_file_path();

// Absolute path to TDLib's database_directory (td.db/) inside the config
// directory. Callers pass this to setTdlibParameters; ensure_database_dir()
// creates it with 0700 perms.
[[nodiscard]] std::string database_dir();

// Load config.json. Returns the parsed Config on success, or an Error:
//   code "config_missing" — file absent (the not_authorized path uses this),
//   code "config_invalid" — present but unreadable/malformed.
[[nodiscard]] std::variant<Config, Error> load_config();

// Save config.json (creating the config directory 0700 if needed), with the
// file itself mode 0600. Returns an Error on any I/O failure.
[[nodiscard]] std::optional<Error> save_config(const Config& config);

// Create the database directory (td.db/) with mode 0700 if it doesn't exist.
// Returns an Error on failure.
[[nodiscard]] std::optional<Error> ensure_database_dir();

} // namespace tgcurl

#endif // TGCURL_CONFIG_H
