// send "<id>" "<text>" — send a text message to a chat.
//
// Build an inputMessageText and hand it to the shared send pipeline
// (send_common.h): sendMessage returns immediately with a message in the
// "pending" state and a temporary id — the message is only queued locally, not
// yet on the server. Because tgcurl is one-shot, the pipeline waits for the
// terminal update for that message (updateMessageSendSucceeded /
// updateMessageSendFailed) before exiting; otherwise the process can quit with
// the send still pending and the message never leaves. Prints
// {"ok":true,"message_id":…} once the server has accepted it.
#include "error.h"
#include "send_common.h"

#include <optional>
#include <ostream>
#include <string>
#include <td/telegram/td_api.h>
#include <variant>

namespace tgcurl {

namespace td_api = td::td_api;

namespace {

// How long to wait for the server to accept a queued message before giving up.
constexpr double kSendTimeoutSeconds = 30.0;

constexpr const char* kUsage = R"(send "<chat_id|@username>" "<text>" )"
                               R"([--reply-to <message_id>] [--silent] [--at <unix_time>])";

} // namespace

namespace commands {

std::optional<Error> send(const Args& args, std::ostream& out) {
    if (args.size() < 2) {
        return Error("usage", kUsage);
    }
    const std::string& id_arg = args[0];
    const std::string& text = args[1];

    std::variant<SendFlags, Error> flags = parse_send_flags(args, 2);
    if (std::holds_alternative<Error>(flags)) {
        return std::get<Error>(flags);
    }

    // Build inputMessageText { formattedText{text, no entities}, default link
    // preview, don't clear draft }. The entities array is left default-empty.
    // As of TDLib 1.8.63 the disable_web_page_preview bool became a
    // link_preview_options object; nullptr keeps TDLib's default (previews on).
    auto formatted = td_api::make_object<td_api::formattedText>();
    formatted->text_ = text;
    auto content = td_api::make_object<td_api::inputMessageText>(
        std::move(formatted), /*link_preview_options=*/nullptr, /*clear_draft=*/false);

    return send_content(id_arg, std::move(content), std::get<SendFlags>(flags), kSendTimeoutSeconds,
                        out);
}

} // namespace commands
} // namespace tgcurl
