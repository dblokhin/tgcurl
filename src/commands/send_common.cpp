#include "send_common.h"

#include "json_out.h"
#include "resolve.h"
#include "send_confirm.h"
#include "session.h"
#include "tdclient.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace tgcurl {

namespace td_api = td::td_api;

namespace {

// Parse a strictly-positive integer flag value; the whole lexeme must be
// consumed (rejects "12abc").
std::optional<std::int64_t> parse_positive(const std::string& value) {
    try {
        std::size_t consumed = 0;
        const std::int64_t parsed = std::stoll(value, &consumed);
        if (consumed != value.size() || parsed <= 0) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

std::variant<SendFlags, Error> parse_send_flags(const Args& args, std::size_t start) {
    SendFlags flags;
    for (std::size_t i = start; i < args.size(); ++i) {
        if (args[i] == "--silent") {
            flags.silent = true;
        } else if (args[i] == "--reply-to") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--reply-to needs a value");
            }
            const std::optional<std::int64_t> parsed = parse_positive(args[++i]);
            if (!parsed) {
                return Error("usage", "--reply-to must be a positive message id");
            }
            flags.reply_to = *parsed;
        } else if (args[i] == "--at") {
            if (i + 1 >= args.size()) {
                return Error("usage", "--at needs a value");
            }
            const std::optional<std::int64_t> parsed = parse_positive(args[++i]);
            // Telegram carries send_date as a 32-bit unix time.
            if (!parsed || *parsed > std::numeric_limits<std::int32_t>::max()) {
                return Error("usage", "--at must be a unix time in seconds (UTC)");
            }
            flags.send_at = *parsed;
        } else {
            return Error("usage", "unknown option: " + args[i]);
        }
    }
    return flags;
}

std::vector<std::string> split_list(const std::string& value) {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t end = value.find('|', start);
        if (end == std::string::npos) {
            end = value.size();
        }
        std::string item = value.substr(start, end - start);
        const std::size_t first = item.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const std::size_t last = item.find_last_not_of(" \t");
            items.push_back(item.substr(first, last - first + 1));
        }
        start = end + 1;
    }
    return items;
}

std::optional<Error> send_content(const std::string& id_arg,
                                  td_api::object_ptr<td_api::InputMessageContent> content,
                                  const SendFlags& flags, double timeout_seconds,
                                  std::ostream& out) {
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

    auto request = td_api::make_object<td_api::sendMessage>();
    request->chat_id_ = chat_id;
    request->input_message_content_ = std::move(content);
    if (flags.reply_to != 0) {
        // Reply to a message in the same chat (id from chat_history/search).
        auto reply = td_api::make_object<td_api::inputMessageReplyToMessage>();
        reply->message_id_ = flags.reply_to;
        request->reply_to_ = std::move(reply);
    }
    if (flags.silent || flags.send_at != 0) {
        auto options = td_api::make_object<td_api::messageSendOptions>();
        options->disable_notification_ = flags.silent;
        if (flags.send_at != 0) {
            // The message lands in the chat's scheduled queue; the server still
            // acks the scheduling itself with updateMessageSendSucceeded, so the
            // shared wait below covers scheduled sends too.
            options->scheduling_state_ =
                td_api::make_object<td_api::messageSchedulingStateSendAtDate>(
                    static_cast<std::int32_t>(flags.send_at), /*repeat_period=*/0);
        }
        request->options_ = std::move(options);
    }

    // Send and wait for the server's acceptance (see send_confirm.h).
    std::variant<std::int64_t, Error> sent =
        send_with_ack(client, std::move(request), timeout_seconds);
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

} // namespace tgcurl
