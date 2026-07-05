// Tests for SendConfirmation — the codified asynchrony rule (DESIGN.md →
// Asynchrony discipline): a sent message counts as sent only after its
// terminal update (updateMessageSendSucceeded / updateMessageSendFailed)
// arrives, and that update must be honored in EITHER order relative to the
// sendMessage response. The "update first" cases below are the regression
// guard for the race where a fast ack was dropped and the send timed out.
#include "send_confirm.h"
#include "test_util.h"

#include <td/telegram/td_api.h>

using namespace tgcurl;
namespace td_api = td::td_api;

namespace {

td_api::object_ptr<td_api::Object> succeeded(std::int64_t old_id) {
    auto upd = td_api::make_object<td_api::updateMessageSendSucceeded>();
    upd->old_message_id_ = old_id;
    return upd;
}

// old_id and code are unlike concepts (a message id vs. an error code); test
// call sites keep them visually distinct.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
td_api::object_ptr<td_api::Object> failed(std::int64_t old_id, std::int32_t code,
                                          const std::string& message) {
    auto upd = td_api::make_object<td_api::updateMessageSendFailed>();
    upd->old_message_id_ = old_id;
    upd->error_ = td_api::make_object<td_api::error>(code, message);
    return upd;
}

} // namespace

int main() {
    // Normal order: response (pending id) first, then the terminal update.
    {
        SendConfirmation c;
        c.set_pending_id(42);
        CHECK(!c.done());
        c.observe(succeeded(42));
        CHECK(c.done());
        CHECK(c.outcome() == SendConfirmation::Outcome::Succeeded);
    }

    // Raced order: the success update is observed BEFORE the pending id is
    // known; set_pending_id must replay it, not drop it.
    {
        SendConfirmation c;
        c.observe(succeeded(42));
        CHECK(!c.done());
        c.set_pending_id(42);
        CHECK(c.done());
        CHECK(c.outcome() == SendConfirmation::Outcome::Succeeded);
    }

    // Raced order with a failure: error text survives the buffering.
    {
        SendConfirmation c;
        c.observe(failed(7, 400, "CHAT_SEND_FORBIDDEN"));
        c.set_pending_id(7);
        CHECK(c.done());
        CHECK(c.outcome() == SendConfirmation::Outcome::Failed);
        CHECK_EQ(c.error(), "400: CHAT_SEND_FORBIDDEN");
    }

    // Failure after the response, with a missing error object.
    {
        SendConfirmation c;
        c.set_pending_id(7);
        auto upd = td_api::make_object<td_api::updateMessageSendFailed>();
        upd->old_message_id_ = 7;
        td_api::object_ptr<td_api::Object> obj = std::move(upd);
        c.observe(obj);
        CHECK(c.done());
        CHECK(c.outcome() == SendConfirmation::Outcome::Failed);
        CHECK_EQ(c.error(), "unknown send error");
    }

    // Updates for other messages never match, in either order.
    {
        SendConfirmation c;
        c.observe(succeeded(1));
        c.set_pending_id(42);
        CHECK(!c.done());
        c.observe(failed(2, 400, "x"));
        CHECK(!c.done());
        c.observe(succeeded(42));
        CHECK(c.done());
    }

    // Unrelated update types and nulls are ignored.
    {
        SendConfirmation c;
        c.set_pending_id(42);
        td_api::object_ptr<td_api::Object> unrelated =
            td_api::make_object<td_api::updateNewMessage>();
        c.observe(unrelated);
        td_api::object_ptr<td_api::Object> null_obj;
        c.observe(null_obj);
        CHECK(!c.done());
    }

    // Once done, later updates don't overwrite the outcome.
    {
        SendConfirmation c;
        c.set_pending_id(42);
        c.observe(succeeded(42));
        c.observe(failed(42, 500, "late"));
        CHECK(c.outcome() == SendConfirmation::Outcome::Succeeded);
        CHECK_EQ(c.error(), "");
    }

    RETURN_TEST_RESULT();
}
