// sendfile "<id>" "<path>" ["<caption>"] — send a local file as a document.
//
// Build an inputMessageDocument over inputFileLocal and hand it to the shared
// send pipeline (send_common.h). TDLib uploads the file and only then the
// terminal updateMessageSendSucceeded arrives — the shared ack wait covers
// the whole upload, so {"ok":true} really means "the server has the file"
// (see DESIGN.md -> Asynchrony discipline). The upload budget is therefore
// much larger than a text send's.
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

// Upload budget: a document upload is bounded by bandwidth, not latency; give
// it minutes, not the seconds a text send needs.
constexpr double kSendFileTimeoutSeconds = 300.0;

constexpr const char* kUsage = R"(sendfile "<chat_id|@username>" "<path>" ["<caption>"] )"
                               R"([--reply-to <message_id>] [--silent] [--at <unix_time>])";

} // namespace

namespace commands {

std::optional<Error> sendfile(const Args& args, std::ostream& out) {
    std::variant<MediaArgs, Error> parsed = parse_media_args(args, kUsage);
    if (std::holds_alternative<Error>(parsed)) {
        return std::get<Error>(parsed);
    }
    auto& ma = std::get<MediaArgs>(parsed);

    auto content = td_api::make_object<td_api::inputMessageDocument>();
    content->document_ = td_api::make_object<td_api::inputFileLocal>(ma.abs_path);
    if (!ma.caption.empty()) {
        auto formatted = td_api::make_object<td_api::formattedText>();
        formatted->text_ = ma.caption;
        content->caption_ = std::move(formatted);
    }

    return send_content(ma.id_arg, std::move(content), ma.flags, kSendFileTimeoutSeconds, out);
}

} // namespace commands
} // namespace tgcurl
