// Shared argument shape of the media senders (sendfile/sendphoto/sendgif):
//   <id> <path> ["<caption>"] [--reply-to <id>] [--silent] [--at <unix_time>]
// Validates the file offline (before any session) and absolutizes the path so
// the result doesn't depend on the daemon's working directory — TDLib resolves
// the path itself.
#ifndef TGCURL_COMMANDS_MEDIA_ARGS_H
#define TGCURL_COMMANDS_MEDIA_ARGS_H

#include "error.h"
#include "send_common.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>

namespace tgcurl {

struct MediaArgs {
    std::string id_arg;
    std::string abs_path;
    std::string caption;
    SendFlags flags;
};

inline std::variant<MediaArgs, Error> parse_media_args(const Args& args, const char* usage) {
    if (args.size() < 2) {
        return Error("usage", usage);
    }
    MediaArgs out;
    out.id_arg = args[0];
    const std::string& path = args[1];

    // The caption is an optional third positional; a "--…" token there is the
    // start of the flags instead.
    std::size_t flags_start = 2;
    if (args.size() >= 3 && args[2].rfind("--", 0) != 0) {
        out.caption = args[2];
        flags_start = 3;
    }
    std::variant<SendFlags, Error> flags = parse_send_flags(args, flags_start);
    if (std::holds_alternative<Error>(flags)) {
        return std::get<Error>(flags);
    }
    out.flags = std::get<SendFlags>(flags);

    // Fail fast, before any session: a missing file can be rejected offline.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return Error("file_not_found", "not a readable file: " + path);
    }
    out.abs_path = std::filesystem::absolute(path, ec).string();
    if (ec) {
        return Error("file_not_found", "cannot resolve path: " + path);
    }
    return out;
}

} // namespace tgcurl

#endif // TGCURL_COMMANDS_MEDIA_ARGS_H
