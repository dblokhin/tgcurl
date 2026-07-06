// chat "<id>" --last N — read the most recent messages of a chat.
//
// Resolve the identifier, load the chat (getChat primes it), then
// getChatHistory(limit=N) from the newest message. Output is a JSON array in
// the shared message shape (see message_render.h), newest-first (the order
// TDLib returns).
#include "error.h"
#include "json_out.h"
#include "message_render.h"
#include "resolve.h"
#include "session.h"
#include "tdclient.h"

#include <algorithm>
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

constexpr int kDefaultLast = 20;
constexpr int kMaxLast = 100;

// Parse `chat "<id>" --last N`. Returns (id_arg, limit) or an Error.
struct ChatArgs {
    std::string id_arg;
    int limit = kDefaultLast;
};

std::variant<ChatArgs, Error> parse_chat_args(const Args& args) {
    if (args.empty()) {
        return Error("usage", R"(chat "<chat_id|@username>" [--last N])");
    }
    ChatArgs out;
    out.id_arg = args[0];
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--last") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--last needs a value");
            }
            const std::string& value = args[i + 1];
            try {
                std::size_t consumed = 0;
                out.limit = std::stoi(value, &consumed);
                if (consumed != value.size() || out.limit <= 0) {
                    return Error("usage", "--last must be a positive integer");
                }
            } catch (const std::exception&) {
                return Error("usage", "--last must be a positive integer");
            }
            ++i;
        } else {
            return Error("usage", "unknown option: " + args[i]);
        }
    }
    out.limit = std::min(out.limit, kMaxLast);
    return out;
}

} // namespace

namespace commands {

std::optional<Error> chat(const Args& args, std::ostream& out) {
    std::variant<ChatArgs, Error> parsed = parse_chat_args(args);
    if (std::holds_alternative<Error>(parsed)) {
        return std::get<Error>(parsed);
    }
    const ChatArgs& ca = std::get<ChatArgs>(parsed);

    // Reject an unresolvable identifier up front — no session needed.
    if (classify(ca.id_arg).kind == IdKind::Unresolvable) {
        return Error("unresolvable",
                     "use chat_id from 'contacts list' / 'chats list', or a public @username");
    }

    std::variant<std::unique_ptr<TdClient>, Error> session = open_session();
    if (std::holds_alternative<Error>(session)) {
        return std::get<Error>(session);
    }
    TdClient& client = *std::get<std::unique_ptr<TdClient>>(session);

    std::variant<std::int64_t, Error> resolved = resolve_id(client, ca.id_arg);
    if (std::holds_alternative<Error>(resolved)) {
        return std::get<Error>(resolved);
    }
    const std::int64_t chat_id = std::get<std::int64_t>(resolved);

    // Prime the chat so history is available; the id was already validated by
    // resolve, so a getChat error here is a real failure.
    Object chat_obj = client.send_query(td_api::make_object<td_api::getChat>(chat_id));
    if (is_error(chat_obj)) {
        return Error("request_failed", "getChat: " + error_text(chat_obj));
    }

    // History from the newest message: from_message_id=0, offset=0.
    auto request = td_api::make_object<td_api::getChatHistory>(
        chat_id, /*from_message_id=*/0, /*offset=*/0, ca.limit, /*only_local=*/false);
    Object hist = client.send_query(std::move(request));
    if (is_error(hist)) {
        return Error("request_failed", "getChatHistory: " + error_text(hist));
    }
    // Safe downcast: getChatHistory returns `messages` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& messages = static_cast<const td_api::messages&>(*hist);

    json::ArrayWriter arr;
    for (const auto& msg : messages.messages_) {
        if (msg != nullptr) {
            arr.element(message_json(*msg));
        }
    }
    json::emit(arr.array(), out);
    return std::nullopt;
}

} // namespace commands
} // namespace tgcurl
