// send "<id>" "<text>" — send a text message to a chat.
//
// Resolve the identifier to a chat_id, then sendMessage with an
// inputMessageText. Prints {"ok":true,"message_id":…}. Note: sendMessage
// returns immediately with a message whose id is the local/temporary one;
// that id is what the caller gets back (sufficient to confirm acceptance).
#include "error.h"
#include "json_out.h"
#include "resolve.h"
#include "session.h"
#include "tdclient.h"

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

    Object result = client.send_query(std::move(request));
    if (is_error(result)) {
        return Error("request_failed", "sendMessage: " + error_text(result));
    }
    // Safe downcast: sendMessage returns `message` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& message = static_cast<const td_api::message&>(*result);

    json::Writer w;
    w.field("ok", true);
    w.field("message_id", static_cast<std::int64_t>(message.id_));
    w.field("chat_id", chat_id);
    json::emit(w.object(), std::cout);
    return std::nullopt;
}

} // namespace commands
} // namespace tgcurl
