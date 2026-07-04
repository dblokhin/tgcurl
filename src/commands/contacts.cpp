// contacts list — dump the address book as JSON.
//
// getContacts returns user_ids; for each we fetch the user (name/username/
// phone) and its private chat_id (the stable identifier every other command
// consumes). Output is a JSON array of
//   {user_id, chat_id, username, phone, first_name, last_name}.
#include "error.h"
#include "json_out.h"
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

namespace {

// Build the JSON object for one contact. chat_id is 0 if its private chat
// couldn't be created (rare; the entry is still emitted so the set is complete).
std::string contact_json(const td_api::user& user, std::int64_t chat_id) {
    json::Writer w;
    w.field("user_id", static_cast<std::int64_t>(user.id_));
    w.field("chat_id", chat_id);
    w.field("username", user.username_);
    w.field("phone", user.phone_number_);
    w.field("first_name", user.first_name_);
    w.field("last_name", user.last_name_);
    return w.object();
}

} // namespace

namespace commands {

std::optional<Error> contacts(const Args& args) {
    // Only the "list" subcommand exists here for now (new/block land in #8).
    if (args.empty() || args[0] != "list") {
        return Error("usage", "contacts list");
    }

    std::variant<std::unique_ptr<TdClient>, Error> session = open_session();
    if (std::holds_alternative<Error>(session)) {
        return std::get<Error>(session);
    }
    TdClient& client = *std::get<std::unique_ptr<TdClient>>(session);

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

    json::emit(arr.array(), std::cout);
    return std::nullopt;
}

} // namespace commands
} // namespace tgcurl
