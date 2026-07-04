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
#include "session.h"
#include "tdclient.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
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

// Outcome of waiting for a sent message's terminal update.
enum class SendOutcome : std::uint8_t { Succeeded, Failed, TimedOut };

// Records the terminal send-result update for our message. The handler is
// installed *before* sendMessage so a fast update can't be dispatched-and-
// dropped before we start listening. updateMessageSendSucceeded /
// updateMessageSendFailed both carry the temporary id in old_message_id_; we
// key on that. seen becomes true once the matching update arrives.
struct SendResult {
    std::int64_t pending_id = 0; // set from the sendMessage response
    bool have_pending_id = false;
    bool seen = false;
    SendOutcome outcome = SendOutcome::TimedOut;
    std::string error;

    void observe(const td_api::object_ptr<td_api::Object>& update) {
        if (seen || !have_pending_id || update == nullptr) {
            return;
        }
        if (update->get_id() == td_api::updateMessageSendSucceeded::ID) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            const auto& upd = static_cast<const td_api::updateMessageSendSucceeded&>(*update);
            if (upd.old_message_id_ == pending_id) {
                outcome = SendOutcome::Succeeded;
                seen = true;
            }
        } else if (update->get_id() == td_api::updateMessageSendFailed::ID) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            const auto& upd = static_cast<const td_api::updateMessageSendFailed&>(*update);
            if (upd.old_message_id_ == pending_id) {
                outcome = SendOutcome::Failed;
                error = upd.error_ != nullptr
                            ? (std::to_string(upd.error_->code_) + ": " + upd.error_->message_)
                            : "unknown send error";
                seen = true;
            }
        }
    }
};

} // namespace

namespace commands {

std::optional<Error> send(const Args& args) {
    if (args.size() != 2) {
        return Error("usage", R"(send "<chat_id|@username>" "<text>")");
    }
    const std::string& id_arg = args[0];
    const std::string& text = args[1];

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

    // Install the result observer before sending so no terminal update can be
    // dispatched and dropped between the response and when we start listening.
    SendResult result_state;
    client.set_update_handler(
        [&](td_api::object_ptr<td_api::Object> update) { result_state.observe(update); });

    Object result = client.send_query(std::move(request));
    if (is_error(result)) {
        return Error("request_failed", "sendMessage: " + error_text(result));
    }
    // Safe downcast: sendMessage returns `message` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& message = static_cast<const td_api::message&>(*result);
    result_state.pending_id = message.id_;
    result_state.have_pending_id = true;

    // The message is only queued so far; wait for the server to accept it before
    // reporting success (and before the process exits, dropping the pending
    // send). Bounded by kSendTimeoutSeconds so a stuck send can't hang forever.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(kSendTimeoutSeconds);
    while (!result_state.seen) {
        const double remaining =
            std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0.0) {
            break;
        }
        client.pump_updates(remaining);
    }

    switch (result_state.outcome) {
    case SendOutcome::Succeeded:
        break;
    case SendOutcome::Failed:
        return Error("request_failed", "sendMessage: " + result_state.error);
    case SendOutcome::TimedOut:
        return Error("request_failed",
                     "sendMessage: timed out waiting for the server to accept the message");
    }

    json::Writer w;
    w.field("ok", true);
    w.field("message_id", result_state.pending_id);
    w.field("chat_id", chat_id);
    json::emit(w.object(), std::cout);
    return std::nullopt;
}

} // namespace commands
} // namespace tgcurl
