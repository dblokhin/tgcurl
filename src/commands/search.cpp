// search "<query>" [--chat <id>] [--limit N] [--offset <cursor>] — find
// messages by text.
//
// With --chat: searchChatMessages inside that chat. Without: searchMessages
// across all chats of the main list. Either way the output is
//   {"total_count": N, "next_offset": "...",
//    "messages": [ ...shared message shape, with chat_id ]}
// newest-first. total_count is the server's estimate of ALL matches; messages
// carries at most --limit of them. next_offset is the pagination cursor: pass
// it back via --offset for the next page; "" means no more results. (In-chat
// it is a message id, globally an opaque server string — callers just echo
// it.) This is the agent's discovery primitive: find context by query instead
// of paging whole histories through the LLM.
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

constexpr int kDefaultLimit = 20;
constexpr int kMaxLimit = 100;
constexpr const char* kUsage =
    R"(search "<query>" [--chat <chat_id|@username>] [--limit N] [--offset <cursor>])";

struct SearchArgs {
    std::string query;
    std::string chat;   // "" = global search
    std::string offset; // "" = first page; else the next_offset of the previous page
    int limit = kDefaultLimit;
};

std::variant<SearchArgs, Error> parse_search_args(const Args& args) {
    if (args.empty() || args[0].empty()) {
        return Error("usage", kUsage);
    }
    SearchArgs out;
    out.query = args[0];
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--chat") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--chat needs a value");
            }
            out.chat = args[++i];
        } else if (args[i] == "--offset") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--offset needs a value");
            }
            out.offset = args[++i];
        } else if (args[i] == "--limit") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--limit needs a value");
            }
            const std::string& value = args[i + 1];
            try {
                std::size_t consumed = 0;
                out.limit = std::stoi(value, &consumed);
                if (consumed != value.size() || out.limit <= 0) {
                    return Error("usage", "--limit must be a positive integer");
                }
            } catch (const std::exception&) {
                return Error("usage", "--limit must be a positive integer");
            }
            ++i;
        } else {
            return Error("usage", "unknown option: " + args[i]);
        }
    }
    out.limit = std::min(out.limit, kMaxLimit);
    return out;
}

// Emit {"total_count":N,"next_offset":"...","messages":[...]}. next_offset
// is "" when there is no further page.
void emit_found(std::int32_t total_count, const std::string& next_offset,
                const std::vector<td_api::object_ptr<td_api::message>>& messages,
                std::ostream& out) {
    json::ArrayWriter arr;
    for (const auto& msg : messages) {
        if (msg != nullptr) {
            arr.element(message_json(*msg, /*with_chat_id=*/true));
        }
    }
    json::Writer w;
    w.field("total_count", total_count);
    w.field("next_offset", next_offset);
    w.raw_field("messages", arr.array());
    json::emit(w.object(), out);
}

std::optional<Error> search_in_chat(TdClient& client, const SearchArgs& sa, std::ostream& out) {
    std::variant<std::int64_t, Error> resolved = resolve_id(client, sa.chat);
    if (std::holds_alternative<Error>(resolved)) {
        return std::get<Error>(resolved);
    }
    // In-chat pagination cursor: the next_from_message_id of the previous
    // page (a message id), searching strictly older results from there.
    std::int64_t from_message_id = 0; // from the newest
    if (!sa.offset.empty()) {
        try {
            std::size_t consumed = 0;
            from_message_id = std::stoll(sa.offset, &consumed);
            if (consumed != sa.offset.size() || from_message_id <= 0) {
                return Error("usage", "--offset must be the next_offset of the previous page");
            }
        } catch (const std::exception&) {
            return Error("usage", "--offset must be the next_offset of the previous page");
        }
    }
    auto request = td_api::make_object<td_api::searchChatMessages>();
    request->chat_id_ = std::get<std::int64_t>(resolved);
    request->query_ = sa.query;
    request->from_message_id_ = from_message_id;
    request->offset_ = 0;
    request->limit_ = sa.limit;
    Object result = client.send_query(std::move(request));
    if (is_error(result)) {
        return Error("request_failed", "searchChatMessages: " + error_text(result));
    }
    // Safe downcast: searchChatMessages returns foundChatMessages on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& found = static_cast<const td_api::foundChatMessages&>(*result);
    const std::string next_offset =
        found.next_from_message_id_ != 0 ? std::to_string(found.next_from_message_id_) : "";
    emit_found(found.total_count_, next_offset, found.messages_, out);
    return std::nullopt;
}

std::optional<Error> search_global(TdClient& client, const SearchArgs& sa, std::ostream& out) {
    auto request = td_api::make_object<td_api::searchMessages>();
    request->chat_list_ = td_api::make_object<td_api::chatListMain>();
    request->query_ = sa.query;
    request->offset_ = sa.offset; // "" = first page; else opaque server cursor
    request->limit_ = sa.limit;
    Object result = client.send_query(std::move(request));
    if (is_error(result)) {
        return Error("request_failed", "searchMessages: " + error_text(result));
    }
    // Safe downcast: searchMessages returns foundMessages on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& found = static_cast<const td_api::foundMessages&>(*result);
    emit_found(found.total_count_, found.next_offset_, found.messages_, out);
    return std::nullopt;
}

} // namespace

namespace commands {

std::optional<Error> search(const Args& args, std::ostream& out) {
    std::variant<SearchArgs, Error> parsed = parse_search_args(args);
    if (std::holds_alternative<Error>(parsed)) {
        return std::get<Error>(parsed);
    }
    const SearchArgs& sa = std::get<SearchArgs>(parsed);

    // Reject an unresolvable chat identifier up front — no session needed.
    if (!sa.chat.empty() && classify(sa.chat).kind == IdKind::Unresolvable) {
        return Error("unresolvable",
                     "use chat_id from 'contacts list' / 'chats list', or a public @username");
    }

    std::variant<std::unique_ptr<TdClient>, Error> session = open_session();
    if (std::holds_alternative<Error>(session)) {
        return std::get<Error>(session);
    }
    TdClient& client = *std::get<std::unique_ptr<TdClient>>(session);

    return sa.chat.empty() ? search_global(client, sa, out) : search_in_chat(client, sa, out);
}

} // namespace commands
} // namespace tgcurl
