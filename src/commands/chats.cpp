// chats list [--limit N] — dump recent dialogs as JSON.
//
// loadChats primes TDLib's in-memory chat list from the server, then getChats
// returns the chat_ids and getChat fills each in. Output is a JSON array of
//   {chat_id, title, type, username}
// covering private chats, groups, supergroups and channels — not just contacts.
#include "error.h"
#include "json_out.h"
#include "session.h"
#include "tdclient.h"

#include <algorithm>
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

constexpr int kDefaultLimit = 50;
constexpr int kMaxLimit = 1000;

// Parse `chats list [--limit N]`. Returns the limit, or an Error on bad args.
std::variant<int, Error> parse_limit(const Args& args) {
    // args[0] is "list"; anything after is options.
    int limit = kDefaultLimit;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--limit") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--limit needs a value");
            }
            const std::string& value = args[i + 1];
            try {
                std::size_t consumed = 0;
                limit = std::stoi(value, &consumed);
                if (consumed != value.size() || limit <= 0) {
                    return Error("usage", "--limit must be a positive integer");
                }
            } catch (const std::exception&) {
                return Error("usage", "--limit must be a positive integer");
            }
            ++i; // consumed the value
        } else {
            return Error("usage", "unknown option: " + args[i]);
        }
    }
    return std::min(limit, kMaxLimit);
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
    return w.object();
}

} // namespace

namespace commands {

std::optional<Error> chats(const Args& args) {
    if (args.empty() || args[0] != "list") {
        return Error("usage", "chats list [--limit N]");
    }
    std::variant<int, Error> lim = parse_limit(args);
    if (std::holds_alternative<Error>(lim)) {
        return std::get<Error>(lim);
    }
    const int limit = std::get<int>(lim);

    std::variant<std::unique_ptr<TdClient>, Error> session = open_session();
    if (std::holds_alternative<Error>(session)) {
        return std::get<Error>(session);
    }
    TdClient& client = *std::get<std::unique_ptr<TdClient>>(session);

    // Prime the in-memory chat list from the server. loadChats returns an error
    // with code 404 once there are no more chats to load — that's expected and
    // not a failure, so we ignore its result and just query what's loaded.
    client.send_query(
        td_api::make_object<td_api::loadChats>(td_api::make_object<td_api::chatListMain>(), limit));

    Object chats_obj = client.send_query(
        td_api::make_object<td_api::getChats>(td_api::make_object<td_api::chatListMain>(), limit));
    if (is_error(chats_obj)) {
        return Error("request_failed", "getChats: " + error_text(chats_obj));
    }
    // Safe downcast: getChats returns `chats` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& chats = static_cast<const td_api::chats&>(*chats_obj);

    json::ArrayWriter arr;
    for (std::int64_t chat_id : chats.chat_ids_) {
        Object chat_obj = client.send_query(td_api::make_object<td_api::getChat>(chat_id));
        if (is_error(chat_obj)) {
            return Error("request_failed", "getChat: " + error_text(chat_obj));
        }
        // Safe downcast: getChat returns `chat` on success.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        arr.element(chat_json(client, static_cast<const td_api::chat&>(*chat_obj)));
    }

    json::emit(arr.array(), std::cout);
    return std::nullopt;
}

} // namespace commands
} // namespace tgcurl
