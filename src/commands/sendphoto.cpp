// sendphoto "<id>" "<path>" ["<caption>"] — send a local image as a photo.
//
// Unlike sendfile (inputMessageDocument, which receivers see as an attached
// file), inputMessagePhoto makes clients render the image inline in the chat.
// Telegram re-compresses photos server-side; to deliver the original bytes,
// use sendfile instead.
//
// Build an inputMessagePhoto over inputFileLocal and hand it to the shared
// send pipeline (send_common.h). TDLib reads the image dimensions itself
// (width_/height_ stay 0); the terminal updateMessageSendSucceeded only
// arrives after the upload, so the shared ack wait covers it all
// (see DESIGN.md -> Asynchrony discipline).
//
// Over MCP the path is read by the tgcurl process — it must be reachable
// from where the server runs (mind Docker mounts).
#include "error.h"
#include "media_args.h"
#include "send_common.h"

#include <optional>
#include <ostream>
#include <td/telegram/td_api.h>
#include <utility>
#include <variant>

namespace tgcurl {

namespace td_api = td::td_api;

namespace {

// Upload budget: same as sendfile — bounded by bandwidth, not latency.
constexpr double kSendPhotoTimeoutSeconds = 300.0;

constexpr const char* kUsage = R"(sendphoto "<chat_id|@username>" "<path>" ["<caption>"] )"
                               R"([--reply-to <message_id>] [--silent] [--at <unix_time>])";

} // namespace

namespace commands {

std::optional<Error> sendphoto(const Args& args, std::ostream& out) {
    std::variant<MediaArgs, Error> parsed = parse_media_args(args, kUsage);
    if (std::holds_alternative<Error>(parsed)) {
        return std::get<Error>(parsed);
    }
    auto& ma = std::get<MediaArgs>(parsed);

    auto content = td_api::make_object<td_api::inputMessagePhoto>();
    content->photo_ = td_api::make_object<td_api::inputFileLocal>(ma.abs_path);
    if (!ma.caption.empty()) {
        auto formatted = td_api::make_object<td_api::formattedText>();
        formatted->text_ = ma.caption;
        content->caption_ = std::move(formatted);
    }

    return send_content(ma.id_arg, std::move(content), ma.flags, kSendPhotoTimeoutSeconds, out);
}

} // namespace commands
} // namespace tgcurl
