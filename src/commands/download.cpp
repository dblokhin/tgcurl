// download "<chat_id>" <message_id> [--output <path>] — download a message's
// media.
//
// Resolve the identifier, load the chat (getChat primes it), getMessage, pull
// the file out of the content (photo/video/document/audio/animation/voice
// note/video note/sticker — see message_render.h's content_file), then
// downloadFile with synchronous=true: the response arrives only once the file
// is fully on disk, so the response IS the completion signal (class 1 in
// DESIGN.md -> Asynchrony discipline — no update wait needed, unlike uploads).
// The file lands in TDLib's cache inside the config dir; --output copies it
// to a stable caller-chosen path (the cache is TDLib's to garbage-collect).
//
// One bounded wait: downloads of large media legitimately take minutes, so
// the query runs with its own generous timeout instead of the default.
#include "error.h"
#include "json_out.h"
#include "message_render.h"
#include "resolve.h"
#include "session.h"
#include "tdclient.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <td/telegram/td_api.h>
#include <variant>
#include <vector>

namespace tgcurl {

using Args = std::vector<std::string>;

namespace td_api = td::td_api;

namespace {

// TDLib download priority is 1..32 (higher = sooner); a one-shot interactive
// command has nothing to yield to.
constexpr int kDownloadPriority = 32;

// Large media takes a while; the default 30s send_query budget is for chatty
// round-trips, not multi-hundred-MB transfers.
constexpr double kDownloadTimeoutSeconds = 600.0;

// Parse `download "<chat_id>" <message_id> [--output <path>]`.
struct DownloadArgs {
    std::string id_arg;
    std::int64_t message_id = 0;
    std::string output; // "" = leave the file in TDLib's cache
};

std::variant<DownloadArgs, Error> parse_download_args(const Args& args) {
    if (args.size() < 2) {
        return Error("usage", R"(download "<chat_id|@username>" <message_id> [--output <path>])");
    }
    DownloadArgs out;
    out.id_arg = args[0];
    try {
        std::size_t consumed = 0;
        out.message_id = std::stoll(args[1], &consumed);
        if (consumed != args[1].size() || out.message_id <= 0) {
            return Error("usage", "message_id must be a positive integer");
        }
    } catch (const std::exception&) {
        return Error("usage", "message_id must be a positive integer");
    }
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--output") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--output needs a path");
            }
            out.output = args[++i];
        } else {
            return Error("usage", "unknown option: " + args[i]);
        }
    }
    return out;
}

} // namespace

namespace commands {

std::optional<Error> download(const Args& args, std::ostream& out) {
    std::variant<DownloadArgs, Error> parsed = parse_download_args(args);
    if (std::holds_alternative<Error>(parsed)) {
        return std::get<Error>(parsed);
    }
    const DownloadArgs& da = std::get<DownloadArgs>(parsed);

    // Reject an unresolvable identifier up front — no session needed.
    if (classify(da.id_arg).kind == IdKind::Unresolvable) {
        return Error("unresolvable",
                     "use chat_id from 'contacts list' / 'chats list', or a public @username");
    }

    std::variant<std::unique_ptr<TdClient>, Error> session = open_session();
    if (std::holds_alternative<Error>(session)) {
        return std::get<Error>(session);
    }
    TdClient& client = *std::get<std::unique_ptr<TdClient>>(session);

    std::variant<std::int64_t, Error> resolved = resolve_id(client, da.id_arg);
    if (std::holds_alternative<Error>(resolved)) {
        return std::get<Error>(resolved);
    }
    const std::int64_t chat_id = std::get<std::int64_t>(resolved);

    // Prime the chat so the message is addressable; the id was already
    // validated by resolve, so a getChat error here is a real failure.
    Object chat_obj = client.send_query(td_api::make_object<td_api::getChat>(chat_id));
    if (is_error(chat_obj)) {
        return Error("request_failed", "getChat: " + error_text(chat_obj));
    }

    Object msg_obj =
        client.send_query(td_api::make_object<td_api::getMessage>(chat_id, da.message_id));
    if (is_error(msg_obj)) {
        return Error("request_failed", "getMessage: " + error_text(msg_obj));
    }
    // Safe downcast: getMessage returns `message` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& msg = static_cast<const td_api::message&>(*msg_obj);

    std::string name;
    const td_api::file* file = content_file(msg.content_.get(), name);
    if (file == nullptr) {
        return Error("no_media", "message " + std::to_string(da.message_id) + " is of type '" +
                                     content_type_name(msg.content_.get()) +
                                     "' and carries no downloadable file");
    }

    // synchronous=true: the response is delivered only after the download
    // finished (or failed) — the blocking semantics this one-shot command
    // wants; no updateFile tracking needed.
    Object dl = client.send_query(
        td_api::make_object<td_api::downloadFile>(file->id_, kDownloadPriority, /*offset=*/0,
                                                  /*limit=*/0, /*synchronous=*/true),
        kDownloadTimeoutSeconds);
    if (is_error(dl)) {
        return Error("request_failed", "downloadFile: " + error_text(dl));
    }
    // Safe downcast: downloadFile returns `file` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& downloaded = static_cast<const td_api::file&>(*dl);
    if (downloaded.local_ == nullptr || !downloaded.local_->is_downloading_completed_ ||
        downloaded.local_->path_.empty()) {
        return Error("request_failed", "downloadFile: file did not finish downloading");
    }

    std::string path = downloaded.local_->path_;
    if (!da.output.empty()) {
        std::error_code ec;
        std::filesystem::copy_file(path, da.output,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return Error("request_failed", "cannot copy to " + da.output + ": " + ec.message());
        }
        path = da.output;
    }

    json::Writer w;
    w.field("ok", true);
    w.field("chat_id", chat_id);
    w.field("message_id", da.message_id);
    w.field("type", content_type_name(msg.content_.get()));
    w.field("name", name);
    w.field("size", static_cast<std::int64_t>(downloaded.size_));
    w.field("path", path);
    json::emit(w.object(), out);
    return std::nullopt;
}

} // namespace commands
} // namespace tgcurl
