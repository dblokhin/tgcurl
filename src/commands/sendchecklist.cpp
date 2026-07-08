// sendchecklist "<id>" "<title>" "<task1|task2|...>" — send a checklist.
//
// inputMessageChecklist (Telegram checklists, mid-2025): a titled to-do list
// whose tasks the participants can tick off. Sending one requires Telegram
// Premium — without it the server rejects the message and the error surfaces
// as this command's result. Tasks arrive as one '|'-separated argument for
// the same reason as sendpoll's options (no array parameters in the
// registry). Recipients may mark tasks done, but not add tasks.
#include "error.h"
#include "send_common.h"

#include <cstdint>
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

// A checklist is a text-sized payload; the text-send budget fits.
constexpr double kSendChecklistTimeoutSeconds = 30.0;

constexpr const char* kUsage =
    R"(sendchecklist "<chat_id|@username>" "<title>" "<task1|task2|...>" )"
    R"([--reply-to <message_id>] [--silent] [--at <unix_time>])";

td_api::object_ptr<td_api::formattedText> plain_text(const std::string& text) {
    auto formatted = td_api::make_object<td_api::formattedText>();
    formatted->text_ = text;
    return formatted;
}

} // namespace

namespace commands {

std::optional<Error> sendchecklist(const Args& args, std::ostream& out) {
    if (args.size() < 3) {
        return Error("usage", kUsage);
    }
    const std::string& id_arg = args[0];
    const std::string& title = args[1];
    if (title.empty()) {
        return Error("usage", "the checklist title must not be empty");
    }
    const std::vector<std::string> tasks = split_list(args[2]);
    if (tasks.empty()) {
        return Error("usage",
                     "a checklist needs at least 1 task, '|'-separated: \"buy milk|call mom\"");
    }
    std::variant<SendFlags, Error> flags = parse_send_flags(args, 3);
    if (std::holds_alternative<Error>(flags)) {
        return std::get<Error>(flags);
    }

    auto checklist = td_api::make_object<td_api::inputChecklist>();
    checklist->title_ = plain_text(title);
    // Task ids only need to be unique within the checklist; 1..N.
    std::int32_t task_id = 1;
    for (const std::string& task : tasks) {
        checklist->tasks_.push_back(
            td_api::make_object<td_api::inputChecklistTask>(task_id++, plain_text(task)));
    }
    checklist->others_can_add_tasks_ = false;
    checklist->others_can_mark_tasks_as_done_ = true;

    auto content = td_api::make_object<td_api::inputMessageChecklist>(std::move(checklist));

    return send_content(id_arg, std::move(content), std::get<SendFlags>(flags),
                        kSendChecklistTimeoutSeconds, out);
}

} // namespace commands
} // namespace tgcurl
