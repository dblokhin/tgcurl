// Tracks whether the server has accepted a message this client sent.
//
// This encodes tgcurl's asynchrony rule for fire-and-forget TDLib requests
// (DESIGN.md → Asynchrony discipline): sendMessage's *response* only means the
// message was queued locally with a temporary id — the send is complete only
// when the matching terminal update (updateMessageSendSucceeded /
// updateMessageSendFailed, keyed by old_message_id_) arrives. A one-shot
// process that exits before that update drops the message.
//
// Two orderings must both work, which is why this is stateful:
//   - normal: sendMessage response (gives the temporary id) → terminal update;
//   - racy:   the terminal update is dispatched while we're still waiting for
//     the response, i.e. before the temporary id is known.
// observe() therefore buffers terminal updates it can't match yet, and
// set_pending_id() replays that buffer. Install the observer *before* issuing
// sendMessage and feed it every update.
//
// Pure logic over td_api objects (no network, no client) so it is unit-tested.
#ifndef TGCURL_SEND_CONFIRM_H
#define TGCURL_SEND_CONFIRM_H

#include <cstdint>
#include <string>
#include <td/telegram/td_api.h>
#include <utility>
#include <vector>

namespace tgcurl {

class SendConfirmation {
  public:
    enum class Outcome : std::uint8_t { Pending, Succeeded, Failed };

    // Feed one incoming update. Safe to call with any update type, at any
    // point (before or after the pending id is known).
    void observe(const td::td_api::object_ptr<td::td_api::Object>& update) {
        if (done() || update == nullptr) {
            return;
        }
        namespace td_api = td::td_api;
        if (update->get_id() == td_api::updateMessageSendSucceeded::ID) {
            // Safe downcast: get_id() matched.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            const auto& upd = static_cast<const td_api::updateMessageSendSucceeded&>(*update);
            consider(Terminal{upd.old_message_id_, Outcome::Succeeded, ""});
        } else if (update->get_id() == td_api::updateMessageSendFailed::ID) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            const auto& upd = static_cast<const td_api::updateMessageSendFailed&>(*update);
            std::string error =
                upd.error_ != nullptr
                    ? (std::to_string(upd.error_->code_) + ": " + upd.error_->message_)
                    : "unknown send error";
            consider(Terminal{upd.old_message_id_, Outcome::Failed, std::move(error)});
        }
    }

    // Record the temporary message id from the sendMessage response, then
    // replay any terminal updates that raced ahead of it.
    void set_pending_id(std::int64_t id) {
        pending_id_ = id;
        have_pending_id_ = true;
        for (const Terminal& t : buffered_) {
            if (done()) {
                break;
            }
            match(t);
        }
        buffered_.clear();
    }

    // True once the terminal update for our message has been seen.
    [[nodiscard]] bool done() const { return outcome_ != Outcome::Pending; }
    [[nodiscard]] Outcome outcome() const { return outcome_; }
    // "<code>: <message>" of the failure; empty unless outcome() == Failed.
    [[nodiscard]] const std::string& error() const { return error_; }

  private:
    struct Terminal {
        std::int64_t old_message_id = 0;
        Outcome outcome = Outcome::Pending;
        std::string error;
    };

    void consider(Terminal t) {
        if (!have_pending_id_) {
            // Response not seen yet — keep it until set_pending_id() replays.
            buffered_.push_back(std::move(t));
            return;
        }
        match(t);
    }

    void match(const Terminal& t) {
        if (t.old_message_id == pending_id_) {
            outcome_ = t.outcome;
            error_ = t.error;
        }
    }

    std::int64_t pending_id_ = 0;
    bool have_pending_id_ = false;
    Outcome outcome_ = Outcome::Pending;
    std::string error_;
    // Terminal updates seen before the pending id was known. Only this
    // client's own sends produce these updates, so the buffer stays tiny.
    std::vector<Terminal> buffered_;
};

} // namespace tgcurl

#endif // TGCURL_SEND_CONFIRM_H
