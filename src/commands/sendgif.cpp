// sendgif "<id>" "<path>" ["<caption>"] — send a GIF/animation.
//
// inputMessageAnimation: clients render it inline and auto-play it in a loop.
// Telegram accepts GIF and soundless MP4 (it converts GIFs to MP4 server-side
// anyway); a file with an audio track lands as a video instead. TDLib detects
// duration/width/height itself (the fields stay 0).
//
// Same pipeline as the other media senders: inputFileLocal + the shared
// send_content() ack wait, which covers the whole upload.
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
constexpr double kSendGifTimeoutSeconds = 300.0;

constexpr const char* kUsage = R"(sendgif "<chat_id|@username>" "<path>" ["<caption>"] )"
                               R"([--reply-to <message_id>] [--silent] [--at <unix_time>])";

} // namespace

namespace commands {

std::optional<Error> sendgif(const Args& args, std::ostream& out) {
    std::variant<MediaArgs, Error> parsed = parse_media_args(args, kUsage);
    if (std::holds_alternative<Error>(parsed)) {
        return std::get<Error>(parsed);
    }
    auto& ma = std::get<MediaArgs>(parsed);

    auto content = td_api::make_object<td_api::inputMessageAnimation>();
    content->animation_ = td_api::make_object<td_api::inputFileLocal>(ma.abs_path);
    if (!ma.caption.empty()) {
        auto formatted = td_api::make_object<td_api::formattedText>();
        formatted->text_ = ma.caption;
        content->caption_ = std::move(formatted);
    }

    return send_content(ma.id_arg, std::move(content), ma.flags, kSendGifTimeoutSeconds, out);
}

} // namespace commands
} // namespace tgcurl
