// send "<id>" "<text>" — send a text message to a chat.
//
// Resolve the identifier to a chat_id, then sendMessage with an
// inputMessageText. sendMessage returns immediately with a message in the
// "pending" state and a temporary id — the message is only queued locally, not
// yet on the server. Because tgcurl is one-shot, we must then wait for the
// terminal update for that message (updateMessageSendSucceeded /
// updateMessageSendFailed) before exiting; otherwise the process can quit with
// the send still pending and the message never leaves. Prints
// {"ok":true,"message_id":…} once the server has accepted it.
#include "error.h"
#include "json_out.h"
#include "resolve.h"
#include "send_confirm.h"
#include "session.h"
#include "tdclient.h"

#include <cstdint>
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

// How long to wait for the server to accept a queued message before giving up.
constexpr double kSendTimeoutSeconds = 30.0;

constexpr const char* kUsage = R"(send "<chat_id|@username>" "<text>" [--reply-to <message_id>])";

struct SendArgs {
    std::string id_arg;
    std::string text;
    std::int64_t reply_to = 0; // 0 = not a reply
};

std::variant<SendArgs, Error> parse_send_args(const Args& args) {
    if (args.size() < 2) {
        return Error("usage", kUsage);
    }
    SendArgs out;
    out.id_arg = args[0];
    out.text = args[1];
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--reply-to") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--reply-to needs a value");
            }
            const std::string& value = args[i + 1];
            try {
                std::size_t consumed = 0;
                out.reply_to = std::stoll(value, &consumed);
                if (consumed != value.size() || out.reply_to <= 0) {
                    return Error("usage", "--reply-to must be a positive message id");
                }
            } catch (const std::exception&) {
                return Error("usage", "--reply-to must be a positive message id");
            }
            ++i;
        } else {
            return Error("usage", "unknown option: " + args[i]);
        }
    }
    return out;
}

} // namespace

namespace commands {

std::optional<Error> send(const Args& args, std::ostream& out) {
    std::variant<SendArgs, Error> parsed = parse_send_args(args);
    if (std::holds_alternative<Error>(parsed)) {
        return std::get<Error>(parsed);
    }
    const SendArgs& sa = std::get<SendArgs>(parsed);
    const std::string& id_arg = sa.id_arg;
    const std::string& text = sa.text;

    // Reject an unresolvable identifier up front — no session needed to know a
    // free-text name can't be addressed.
    if (classify(id_arg).kind == IdKind::Unresolvable) {
        return Error("unresolvable",
                     "use chat_id from 'contacts list' / 'chats list', or a public @username");
    }

    std::variant<std::unique_ptr<TdClient>, Error> session = open_session();
    if (std::holds_alternative<Error>(session)) {
        return std::get<Error>(session);
    }
    TdClient& client = *std::get<std::unique_ptr<TdClient>>(session);

    std::variant<std::int64_t, Error> resolved = resolve_id(client, id_arg);
    if (std::holds_alternative<Error>(resolved)) {
        return std::get<Error>(resolved);
    }
    const std::int64_t chat_id = std::get<std::int64_t>(resolved);

    // Build inputMessageText { formattedText{text, no entities}, default link
    // preview, don't clear draft }. The entities array is left default-empty.
    // As of TDLib 1.8.63 the disable_web_page_preview bool became a
    // link_preview_options object; nullptr keeps TDLib's default (previews on).
    auto formatted = td_api::make_object<td_api::formattedText>();
    formatted->text_ = text;
    auto content = td_api::make_object<td_api::inputMessageText>(
        std::move(formatted), /*link_preview_options=*/nullptr, /*clear_draft=*/false);

    auto request = td_api::make_object<td_api::sendMessage>();
    request->chat_id_ = chat_id;
    request->input_message_content_ = std::move(content);
    if (sa.reply_to != 0) {
        // Reply to a message in the same chat (id from chat_history/search).
        auto reply = td_api::make_object<td_api::inputMessageReplyToMessage>();
        reply->message_id_ = sa.reply_to;
        request->reply_to_ = std::move(reply);
    }

    // Send and wait for the server's acceptance (see send_confirm.h).
    std::variant<std::int64_t, Error> sent =
        send_with_ack(client, std::move(request), kSendTimeoutSeconds);
    if (std::holds_alternative<Error>(sent)) {
        return std::get<Error>(sent);
    }

    json::Writer w;
    w.field("ok", true);
    w.field("message_id", std::get<std::int64_t>(sent));
    w.field("chat_id", chat_id);
    json::emit(w.object(), out);
    return std::nullopt;
}

} // namespace commands
} // namespace tgcurl
