#include "resolve.h"

#include <cctype>
#include <string>
#include <td/telegram/td_api.h>
#include <variant>

namespace tgcurl {

namespace td_api = td::td_api;

Classified classify(const std::string& arg) {
    if (arg.empty()) {
        return Classified{IdKind::Unresolvable, 0, ""};
    }

    // '@username' → public username (strip the '@'; reject a bare '@').
    if (arg[0] == '@') {
        std::string name = arg.substr(1);
        if (name.empty()) {
            return Classified{IdKind::Unresolvable, 0, ""};
        }
        return Classified{IdKind::Username, 0, name};
    }

    // Purely numeric (optionally a leading '-', since chat_ids for groups and
    // channels are negative) → a chat_id used directly.
    std::size_t start = (arg[0] == '-') ? 1 : 0;
    if (start < arg.size()) {
        bool all_digits = true;
        for (std::size_t i = start; i < arg.size(); ++i) {
            if (std::isdigit(static_cast<unsigned char>(arg[i])) == 0) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            try {
                std::size_t consumed = 0;
                std::int64_t id = std::stoll(arg, &consumed);
                if (consumed == arg.size()) {
                    return Classified{IdKind::ChatId, id, ""};
                }
            } catch (const std::exception&) {
                return Classified{IdKind::Unresolvable, 0, ""}; // overflowed int64
            }
        }
    }

    return Classified{IdKind::Unresolvable, 0, ""};
}

std::variant<std::int64_t, Error> resolve_id(TdClient& client, const std::string& arg) {
    Classified c = classify(arg);
    switch (c.kind) {
    case IdKind::ChatId: {
        // getChat is a local lookup (it does NOT fetch from the server). It
        // succeeds only if the chat is "warm" — its peer data is in the cache,
        // either fetched earlier this process or persisted from a prior run via
        // use_message_database (see DESIGN.md → Peer identification → Warming).
        // Doing it here validates the id and turns a cold chat into a clear
        // resolve-time error instead of a downstream sendMessage "Chat not
        // found". Works for every chat type (private/group/supergroup/channel).
        Object obj = client.send_query(td_api::make_object<td_api::getChat>(c.chat_id));
        if (is_error(obj)) {
            return Error("unresolvable", "chat " + std::to_string(c.chat_id) +
                                             " not known — run 'chats list' or 'contacts list' "
                                             "first, or use @username: " +
                                             error_text(obj));
        }
        // Safe downcast: getChat returns `chat` on success.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return static_cast<const td_api::chat&>(*obj).id_;
    }

    case IdKind::Username: {
        Object obj = client.send_query(td_api::make_object<td_api::searchPublicChat>(c.username));
        if (is_error(obj)) {
            return Error("unresolvable", "no public chat @" + c.username + ": " + error_text(obj));
        }
        // Safe downcast: searchPublicChat returns `chat` on success.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return static_cast<const td_api::chat&>(*obj).id_;
    }

    case IdKind::Unresolvable:
    default:
        return Error("unresolvable",
                     "use chat_id from 'contacts list' / 'chats list', or a public @username");
    }
}

} // namespace tgcurl
