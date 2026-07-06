// read "<id>" — mark a chat as read.
//
// Resolve the identifier, fetch the newest message (getChatHistory limit=1),
// then viewMessages(force_read=true) on it — Telegram marks everything up to
// the viewed message as read, so viewing the newest clears the chat's unread
// counter. force_read makes it count even though no UI is showing the chat.
// viewMessages is a round-trip request (the `ok` response IS the server ack —
// class 1 in DESIGN.md -> Asynchrony discipline), so no update wait is needed.
//
// The agent loop this completes: chats list --unread -> chat/search ->
// react -> read (so the next --unread pass doesn't resurface it).
#include "error.h"
#include "json_out.h"
#include "resolve.h"
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

namespace commands {

std::optional<Error> read(const Args& args, std::ostream& out) {
    if (args.size() != 1) {
        return Error("usage", R"(read "<chat_id|@username>")");
    }
    const std::string& id_arg = args[0];

    // Reject an unresolvable identifier up front — no session needed.
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

    // The newest message; reading it reads the whole chat.
    auto history = td_api::make_object<td_api::getChatHistory>(
        chat_id, /*from_message_id=*/0, /*offset=*/0, /*limit=*/1, /*only_local=*/false);
    Object hist = client.send_query(std::move(history));
    if (is_error(hist)) {
        return Error("request_failed", "getChatHistory: " + error_text(hist));
    }
    // Safe downcast: getChatHistory returns `messages` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& messages = static_cast<const td_api::messages&>(*hist);

    std::int64_t newest_id = 0;
    for (const auto& msg : messages.messages_) {
        if (msg != nullptr) {
            newest_id = msg->id_;
            break;
        }
    }
    if (newest_id != 0) {
        auto view = td_api::make_object<td_api::viewMessages>();
        view->chat_id_ = chat_id;
        view->message_ids_.push_back(newest_id);
        view->force_read_ = true;
        Object result = client.send_query(std::move(view));
        if (is_error(result)) {
            return Error("request_failed", "viewMessages: " + error_text(result));
        }
    }
    // An empty chat has nothing to read; that's still success.

    json::Writer w;
    w.field("ok", true);
    w.field("chat_id", chat_id);
    w.field("read_up_to", newest_id);
    json::emit(w.object(), out);
    return std::nullopt;
}

} // namespace commands
} // namespace tgcurl
