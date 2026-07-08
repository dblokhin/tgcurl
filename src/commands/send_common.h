// Shared machinery for every send* command.
//
// All senders share the same tail: optional flags (--reply-to, --silent,
// --at), offline id validation, session, resolve, sendMessage with
// messageSendOptions, the server-ack wait (send_confirm.h) and the
// {ok, message_id, chat_id} result. A command builds its InputMessageContent
// and hands the rest to send_content(); parse_send_flags() consumes the
// trailing flags so each command only parses its own positionals.
#ifndef TGCURL_COMMANDS_SEND_COMMON_H
#define TGCURL_COMMANDS_SEND_COMMON_H

#include "error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <td/telegram/td_api.h>
#include <variant>
#include <vector>

namespace tgcurl {

using Args = std::vector<std::string>;

// The send modifiers every send* command accepts.
struct SendFlags {
    std::int64_t reply_to = 0; // 0 = not a reply
    bool silent = false;       // true = no recipient notification
    std::int64_t send_at = 0;  // unix time to schedule the send; 0 = now
};

// Parse the trailing "--reply-to <id> | --silent | --at <unix_time>" flags
// starting at args[start]. Anything else is a usage error.
std::variant<SendFlags, Error> parse_send_flags(const Args& args, std::size_t start);

// Split a '|'-separated list argument (poll options, checklist tasks) into
// its trimmed items; empty items are dropped. "a| b |c" -> {"a","b","c"}.
std::vector<std::string> split_list(const std::string& value);

// The shared send pipeline: validate the id offline, open the session,
// resolve, sendMessage(content) with the flags applied, wait for the server
// ack, print {ok, message_id, chat_id}. `timeout_seconds` bounds the whole
// wait (uploads need minutes, text-like sends seconds).
std::optional<Error> send_content(const std::string& id_arg,
                                  td::td_api::object_ptr<td::td_api::InputMessageContent> content,
                                  const SendFlags& flags, double timeout_seconds,
                                  std::ostream& out);

} // namespace tgcurl

#endif // TGCURL_COMMANDS_SEND_COMMON_H
