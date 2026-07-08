// sendpoll "<id>" "<question>" "<opt1|opt2|...>" — send a regular poll.
//
// inputMessagePoll: an anonymous, single-answer, regular (non-quiz) poll —
// the common default. Answer options arrive as one '|'-separated argument
// because the registry maps MCP arguments 1:1 onto CLI tokens and has no
// array parameters (see registry.h). At least 2 options are required
// (validated offline); the server enforces the upper bound and where polls
// are allowed (groups/channels — Telegram rejects polls in private chats).
#include "error.h"
#include "send_common.h"

#include <optional>
#include <ostream>
#include <string>
#include <td/telegram/td_api.h>
#include <utility>
#include <variant>
#include <vector>

namespace tgcurl {

namespace td_api = td::td_api;

namespace {

// A poll is a text-sized payload; the text-send budget fits.
constexpr double kSendPollTimeoutSeconds = 30.0;

constexpr const char* kUsage = R"(sendpoll "<chat_id|@username>" "<question>" "<opt1|opt2|...>" )"
                               R"([--reply-to <message_id>] [--silent] [--at <unix_time>])";

td_api::object_ptr<td_api::formattedText> plain_text(const std::string& text) {
    auto formatted = td_api::make_object<td_api::formattedText>();
    formatted->text_ = text;
    return formatted;
}

} // namespace

namespace commands {

std::optional<Error> sendpoll(const Args& args, std::ostream& out) {
    if (args.size() < 3) {
        return Error("usage", kUsage);
    }
    const std::string& id_arg = args[0];
    const std::string& question = args[1];
    if (question.empty()) {
        return Error("usage", "the poll question must not be empty");
    }
    const std::vector<std::string> options = split_list(args[2]);
    if (options.size() < 2) {
        return Error("usage", "a poll needs at least 2 options, '|'-separated: \"yes|no\"");
    }
    std::variant<SendFlags, Error> flags = parse_send_flags(args, 3);
    if (std::holds_alternative<Error>(flags)) {
        return std::get<Error>(flags);
    }

    auto content = td_api::make_object<td_api::inputMessagePoll>();
    content->question_ = plain_text(question);
    for (const std::string& option : options) {
        content->options_.push_back(
            td_api::make_object<td_api::inputPollOption>(plain_text(option)));
    }
    content->is_anonymous_ = true;
    content->type_ =
        td_api::make_object<td_api::inputPollTypeRegular>(/*allow_adding_options=*/false);

    return send_content(id_arg, std::move(content), std::get<SendFlags>(flags),
                        kSendPollTimeoutSeconds, out);
}

} // namespace commands
} // namespace tgcurl
