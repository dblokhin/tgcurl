// contacts list / new / block.
//
// list  — getContacts → per user getUser + createPrivateChat → JSON array of
//         {user_id, chat_id, username, phone, first_name, last_name}.
// new   — importContacts(phone, first, [last]) → {ok, user_id, chat_id}.
// block — resolve id → setMessageSenderBlockList(blockListMain) → {ok:true}.
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
#include <utility>
#include <variant>
#include <vector>

namespace tgcurl {

using Args = std::vector<std::string>;

namespace td_api = td::td_api;

namespace {

// Build the JSON object for one contact. chat_id is 0 if its private chat
// couldn't be created (rare; the entry is still emitted so the set is complete).
std::string contact_json(const td_api::user& user, std::int64_t chat_id) {
    json::Writer w;
    w.field("user_id", static_cast<std::int64_t>(user.id_));
    w.field("chat_id", chat_id);
    w.field("username", primary_username(user.usernames_));
    w.field("phone", user.phone_number_);
    w.field("first_name", user.first_name_);
    w.field("last_name", user.last_name_);
    return w.object();
}

// contacts list: dump the whole address book.
std::optional<Error> do_list(TdClient& client, std::ostream& out) {
    Object contacts_obj = client.send_query(td_api::make_object<td_api::getContacts>());
    if (is_error(contacts_obj)) {
        return Error("request_failed", "getContacts: " + error_text(contacts_obj));
    }
    // Safe downcast: getContacts returns `users` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& users = static_cast<const td_api::users&>(*contacts_obj);

    json::ArrayWriter arr;
    for (std::int64_t user_id : users.user_ids_) {
        Object user_obj = client.send_query(td_api::make_object<td_api::getUser>(user_id));
        if (is_error(user_obj)) {
            return Error("request_failed", "getUser: " + error_text(user_obj));
        }
        // Safe downcast: getUser returns `user` on success.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& user = static_cast<const td_api::user&>(*user_obj);

        // Resolve the private chat_id (force=false: don't create server-side if
        // it doesn't already exist; we just want the id).
        std::int64_t chat_id = 0;
        Object chat_obj = client.send_query(
            td_api::make_object<td_api::createPrivateChat>(user_id, /*force=*/false));
        if (!is_error(chat_obj)) {
            // Safe downcast: createPrivateChat returns `chat` on success.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            chat_id = static_cast<const td_api::chat&>(*chat_obj).id_;
        }

        arr.element(contact_json(user, chat_id));
    }

    json::emit(arr.array(), out);
    return std::nullopt;
}

// contacts new <phone> <first> [last]: import a single contact.
std::optional<Error> do_new(TdClient& client, const Args& args, std::ostream& out) {
    // args: ["new", <phone>, <first>, [last]]
    if (args.size() < 3 || args.size() > 4) {
        return Error("usage", "contacts new <phone> <first_name> [last_name]");
    }
    const std::string& phone = args[1];
    const std::string& first = args[2];
    const std::string last = args.size() == 4 ? args[3] : std::string();

    // As of TDLib 1.8.63 importContacts takes `importedContact` (was `contact`);
    // the phone/name fields are unchanged, plus an optional formatted-text note.
    auto contact = td_api::make_object<td_api::importedContact>();
    contact->phone_number_ = phone;
    contact->first_name_ = first;
    contact->last_name_ = last;

    std::vector<td_api::object_ptr<td_api::importedContact>> list;
    list.push_back(std::move(contact));

    Object result = client.send_query(td_api::make_object<td_api::importContacts>(std::move(list)));
    if (is_error(result)) {
        return Error("request_failed", "importContacts: " + error_text(result));
    }
    // Safe downcast: importContacts returns `importedContacts` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& imported = static_cast<const td_api::importedContacts&>(*result);

    // user_id is 0 when the phone isn't on Telegram; still report ok so the
    // caller can distinguish "added but not registered" from a request error.
    std::int64_t user_id = imported.user_ids_.empty() ? 0 : imported.user_ids_.front();
    std::int64_t chat_id = 0;
    if (user_id != 0) {
        Object chat_obj = client.send_query(
            td_api::make_object<td_api::createPrivateChat>(user_id, /*force=*/false));
        if (!is_error(chat_obj)) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            chat_id = static_cast<const td_api::chat&>(*chat_obj).id_;
        }
    }

    json::Writer w;
    w.field("ok", true);
    w.field("user_id", user_id);
    w.field("chat_id", chat_id);
    json::emit(w.object(), out);
    return std::nullopt;
}

// Build the MessageSender for a resolved chat_id: a private chat blocks its
// user; any other chat blocks the chat sender itself.
td_api::object_ptr<td_api::MessageSender> sender_for_chat(TdClient& client, std::int64_t chat_id) {
    Object chat_obj = client.send_query(td_api::make_object<td_api::getChat>(chat_id));
    if (!is_error(chat_obj)) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& chat = static_cast<const td_api::chat&>(*chat_obj);
        if (chat.type_ != nullptr && chat.type_->get_id() == td_api::chatTypePrivate::ID) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            const auto& priv = static_cast<const td_api::chatTypePrivate&>(*chat.type_);
            return td_api::make_object<td_api::messageSenderUser>(priv.user_id_);
        }
    }
    return td_api::make_object<td_api::messageSenderChat>(chat_id);
}

// contacts block <id>: block a chat_id or @username.
std::optional<Error> do_block(TdClient& client, const Args& args, std::ostream& out) {
    // args: ["block", <id>]
    if (args.size() != 2) {
        return Error("usage", "contacts block <chat_id|@username>");
    }
    std::variant<std::int64_t, Error> resolved = resolve_id(client, args[1]);
    if (std::holds_alternative<Error>(resolved)) {
        return std::get<Error>(resolved);
    }
    const std::int64_t chat_id = std::get<std::int64_t>(resolved);

    auto sender = sender_for_chat(client, chat_id);
    // As of TDLib 1.8.63 toggleMessageSenderIsBlocked was replaced by
    // setMessageSenderBlockList; blockListMain is the ordinary block list
    // (blockListStories is the separate "block from stories" list).
    Object result = client.send_query(td_api::make_object<td_api::setMessageSenderBlockList>(
        std::move(sender), td_api::make_object<td_api::blockListMain>()));
    if (is_error(result)) {
        return Error("request_failed", "setMessageSenderBlockList: " + error_text(result));
    }

    json::Writer w;
    w.field("ok", true);
    json::emit(w.object(), out);
    return std::nullopt;
}

} // namespace

namespace commands {

std::optional<Error> contacts(const Args& args, std::ostream& out) {
    if (args.empty()) {
        return Error("usage", "contacts <list|new|block> ...");
    }
    const std::string& sub = args[0];

    // block classifies its identifier before opening a session, so a free-text
    // name fails as unresolvable without needing auth.
    if (sub == "block" && args.size() == 2 && classify(args[1]).kind == IdKind::Unresolvable) {
        return Error("unresolvable",
                     "use chat_id from 'contacts list' / 'chats list', or a public @username");
    }

    if (sub != "list" && sub != "new" && sub != "block") {
        return Error("usage", "contacts <list|new|block> ...");
    }

    // Validate argument shapes before opening a session, so obvious usage
    // mistakes fail fast without needing auth.
    if (sub == "new" && (args.size() < 3 || args.size() > 4)) {
        return Error("usage", "contacts new <phone> <first_name> [last_name]");
    }
    if (sub == "block" && args.size() != 2) {
        return Error("usage", "contacts block <chat_id|@username>");
    }

    std::variant<std::unique_ptr<TdClient>, Error> session = open_session();
    if (std::holds_alternative<Error>(session)) {
        return std::get<Error>(session);
    }
    TdClient& client = *std::get<std::unique_ptr<TdClient>>(session);

    if (sub == "list") {
        return do_list(client, out);
    }
    if (sub == "new") {
        return do_new(client, args, out);
    }
    return do_block(client, args, out);
}

} // namespace commands
} // namespace tgcurl
