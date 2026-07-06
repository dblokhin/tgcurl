// chats list [--limit N] [--offset N] [--unread] — dump recent dialogs as JSON.
//
// loadChats primes TDLib's in-memory chat list from the server, then getChats
// returns the chat_ids and getChat fills each in. Output is a JSON array of
//   {chat_id, title, type, username, unread_count, last_message}
// covering private chats, groups, supergroups and channels — not just
// contacts. --unread keeps only chats with unread messages (or marked
// unread): the agent's "what needs attention" view. --offset skips the first
// N chats of the (recency-ordered) list — the pagination cursor: page with
// --offset <previous offset + limit>. The offset indexes the raw list, before
// the --unread filter, so it stays stable across pages.
#include "error.h"
#include "json_out.h"
#include "message_render.h"
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

constexpr int kDefaultLimit = 50;
constexpr int kMaxLimit = 1000;

struct ChatsArgs {
    int limit = kDefaultLimit;
    int offset = 0;
    bool unread_only = false;
};

// A non-negative integer option value, or nullopt when malformed.
std::optional<int> parse_non_negative(const std::string& value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size() || parsed < 0) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Parse `chats list [--limit N] [--offset N] [--unread]`.
std::variant<ChatsArgs, Error> parse_chats_args(const Args& args) {
    // args[0] is "list"; anything after is options.
    ChatsArgs out;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--limit") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--limit needs a value");
            }
            std::optional<int> limit = parse_non_negative(args[++i]);
            if (!limit.has_value() || *limit == 0) {
                return Error("usage", "--limit must be a positive integer");
            }
            out.limit = *limit;
        } else if (args[i] == "--offset") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--offset needs a value");
            }
            std::optional<int> offset = parse_non_negative(args[++i]);
            if (!offset.has_value()) {
                return Error("usage", "--offset must be a non-negative integer");
            }
            out.offset = *offset;
        } else if (args[i] == "--unread") {
            out.unread_only = true;
        } else {
            return Error("usage", "unknown option: " + args[i]);
        }
    }
    out.limit = std::min(out.limit, kMaxLimit);
    // offset + limit is what actually gets loaded; keep the whole window
    // inside the hard cap so paging can't grow the fetch without bound.
    if (out.offset > kMaxLimit - out.limit) {
        return Error("usage", "--offset + --limit must not exceed " + std::to_string(kMaxLimit));
    }
    return out;
}

// Human-readable type tag for a chat's ChatType.
std::string type_name(const td_api::ChatType& type) {
    switch (type.get_id()) {
    case td_api::chatTypePrivate::ID:
        return "private";
    case td_api::chatTypeBasicGroup::ID:
        return "basic_group";
    case td_api::chatTypeSupergroup::ID:
        // Safe downcast: get_id() matched chatTypeSupergroup.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return static_cast<const td_api::chatTypeSupergroup&>(type).is_channel_ ? "channel"
                                                                                : "supergroup";
    case td_api::chatTypeSecret::ID:
        return "secret";
    default:
        return "unknown";
    }
}

// Best-effort public @username for a chat: users and supergroups/channels can
// have one; groups and secret chats don't. Empty string when there is none or
// the lookup fails (username is a convenience field, never load-bearing).
std::string chat_username(TdClient& client, const td_api::ChatType& type) {
    switch (type.get_id()) {
    case td_api::chatTypePrivate::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        std::int64_t user_id = static_cast<const td_api::chatTypePrivate&>(type).user_id_;
        Object obj = client.send_query(td_api::make_object<td_api::getUser>(user_id));
        if (is_error(obj)) {
            return "";
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return primary_username(static_cast<const td_api::user&>(*obj).usernames_);
    }
    case td_api::chatTypeSupergroup::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        std::int64_t sg_id = static_cast<const td_api::chatTypeSupergroup&>(type).supergroup_id_;
        Object obj = client.send_query(td_api::make_object<td_api::getSupergroup>(sg_id));
        if (is_error(obj)) {
            return "";
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return primary_username(static_cast<const td_api::supergroup&>(*obj).usernames_);
    }
    default:
        return "";
    }
}

std::string chat_json(TdClient& client, const td_api::chat& chat) {
    json::Writer w;
    w.field("chat_id", static_cast<std::int64_t>(chat.id_));
    w.field("title", chat.title_);
    if (chat.type_ != nullptr) {
        w.field("type", type_name(*chat.type_));
        w.field("username", chat_username(client, *chat.type_));
    } else {
        w.field("type", "unknown");
        w.field("username", "");
    }
    w.field("unread_count", chat.unread_count_);
    // The newest message in the shared message shape; null when the chat is
    // empty (or TDLib hasn't loaded it).
    w.raw_field("last_message",
                chat.last_message_ != nullptr ? message_json(*chat.last_message_) : "null");
    return w.object();
}

// A chat "needs attention": unread messages, or manually marked unread.
bool is_unread(const td_api::chat& chat) {
    return chat.unread_count_ > 0 || chat.is_marked_as_unread_;
}

} // namespace

namespace commands {

std::optional<Error> chats(const Args& args, std::ostream& out) {
    if (args.empty() || args[0] != "list") {
        return Error("usage", "chats list [--limit N] [--offset N] [--unread]");
    }
    std::variant<ChatsArgs, Error> parsed = parse_chats_args(args);
    if (std::holds_alternative<Error>(parsed)) {
        return std::get<Error>(parsed);
    }
    const ChatsArgs& ca = std::get<ChatsArgs>(parsed);
    // getChats has no server-side offset, so load the whole window and skip
    // the first `offset` ids locally; parse capped offset+limit at kMaxLimit.
    const int window = ca.offset + ca.limit;

    std::variant<std::unique_ptr<TdClient>, Error> session = open_session();
    if (std::holds_alternative<Error>(session)) {
        return std::get<Error>(session);
    }
    TdClient& client = *std::get<std::unique_ptr<TdClient>>(session);

    // Prime the in-memory chat list from the server. loadChats returns an error
    // with code 404 once there are no more chats to load — that's expected and
    // not a failure, so we ignore its result and just query what's loaded.
    client.send_query(td_api::make_object<td_api::loadChats>(
        td_api::make_object<td_api::chatListMain>(), window));

    Object chats_obj = client.send_query(
        td_api::make_object<td_api::getChats>(td_api::make_object<td_api::chatListMain>(), window));
    if (is_error(chats_obj)) {
        return Error("request_failed", "getChats: " + error_text(chats_obj));
    }
    // Safe downcast: getChats returns `chats` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& chats = static_cast<const td_api::chats&>(*chats_obj);

    json::ArrayWriter arr;
    for (auto idx = static_cast<std::size_t>(ca.offset); idx < chats.chat_ids_.size(); ++idx) {
        const std::int64_t chat_id = chats.chat_ids_[idx];
        Object chat_obj = client.send_query(td_api::make_object<td_api::getChat>(chat_id));
        if (is_error(chat_obj)) {
            return Error("request_failed", "getChat: " + error_text(chat_obj));
        }
        // Safe downcast: getChat returns `chat` on success.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& chat = static_cast<const td_api::chat&>(*chat_obj);
        if (ca.unread_only && !is_unread(chat)) {
            continue;
        }
        arr.element(chat_json(client, chat));
    }

    json::emit(arr.array(), out);
    return std::nullopt;
}

} // namespace commands
} // namespace tgcurl
