// sendphoto "<id>" "<path>" ["<caption>"] — send a local image as a photo.
//
// Unlike sendfile (inputMessageDocument, which receivers see as an attached
// file), inputMessagePhoto makes clients render the image inline in the chat.
// Telegram re-compresses photos server-side; to deliver the original bytes,
// use sendfile instead.
//
// Resolve the identifier, then sendMessage with inputMessagePhoto over
// inputFileLocal. TDLib reads the image dimensions itself (width_/height_
// stay 0) and uploads the file; the terminal updateMessageSendSucceeded only
// arrives after the upload, so the shared send_with_ack() wait covers it all
// (see DESIGN.md -> Asynchrony discipline).
//
// Over MCP the path is read by the tgcurl process — it must be reachable
// from where the server runs (mind Docker mounts).
#include "error.h"
#include "json_out.h"
#include "resolve.h"
#include "send_confirm.h"
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

// Upload budget: same as sendfile — bounded by bandwidth, not latency.
constexpr double kSendPhotoTimeoutSeconds = 300.0;

} // namespace

namespace commands {

std::optional<Error> sendphoto(const Args& args, std::ostream& out) {
    if (args.size() < 2 || args.size() > 3) {
        return Error("usage", R"(sendphoto "<chat_id|@username>" "<path>" ["<caption>"])");
    }
    const std::string& id_arg = args[0];
    const std::string& path = args[1];
    const std::string caption = args.size() == 3 ? args[2] : std::string();

    // Fail fast, before any session: an unaddressable target or a missing
    // file can be rejected offline.
    if (classify(id_arg).kind == IdKind::Unresolvable) {
        return Error("unresolvable",
                     "use chat_id from 'contacts list' / 'chats list', or a public @username");
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return Error("file_not_found", "not a readable file: " + path);
    }
    // TDLib resolves the path itself; make it absolute so the result doesn't
    // depend on the daemon's working directory.
    const std::string abs_path = std::filesystem::absolute(path, ec).string();
    if (ec) {
        return Error("file_not_found", "cannot resolve path: " + path);
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

    auto content = td_api::make_object<td_api::inputMessagePhoto>();
    content->photo_ = td_api::make_object<td_api::inputFileLocal>(abs_path);
    if (!caption.empty()) {
        auto formatted = td_api::make_object<td_api::formattedText>();
        formatted->text_ = caption;
        content->caption_ = std::move(formatted);
    }

    auto request = td_api::make_object<td_api::sendMessage>();
    request->chat_id_ = chat_id;
    request->input_message_content_ = std::move(content);

    // Send and wait until the upload is complete and the server accepted the
    // message (see send_confirm.h).
    std::variant<std::int64_t, Error> sent =
        send_with_ack(client, std::move(request), kSendPhotoTimeoutSeconds);
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
