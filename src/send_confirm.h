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

#include "error.h"
#include "tdclient.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <td/telegram/td_api.h>
#include <utility>
#include <variant>
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

// The full send-with-acknowledgement pattern in one call: install the
// observer, issue the sendMessage, then pump updates until the server's
// terminal update arrives (or the deadline passes). Returns the message id
// or an Error. Every command that sends a message goes through this.
inline std::variant<std::int64_t, Error>
send_with_ack(TdClient& client, td::td_api::object_ptr<td::td_api::sendMessage> request,
              double timeout_seconds) {
    namespace td_api = td::td_api;

    // Install the confirmation observer before sending: it buffers a terminal
    // update even if that update is dispatched while we're still waiting for
    // the sendMessage response itself.
    SendConfirmation confirm;
    client.set_update_handler(
        [&confirm](td_api::object_ptr<td_api::Object> update) { confirm.observe(update); });

    Object result = client.send_query(std::move(request), timeout_seconds);
    if (is_error(result)) {
        return Error("request_failed", "sendMessage: " + error_text(result));
    }
    // Safe downcast: sendMessage returns `message` on success.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const std::int64_t pending_id = static_cast<const td_api::message&>(*result).id_;
    confirm.set_pending_id(pending_id);

    // The message is only queued so far; wait for the server to accept it
    // before reporting success (and before the process exits, dropping the
    // pending send). Bounded so a stuck send can't hang forever.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_seconds);
    while (!confirm.done()) {
        const double remaining =
            std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0.0) {
            break;
        }
        client.pump_updates(remaining);
    }

    switch (confirm.outcome()) {
    case SendConfirmation::Outcome::Succeeded:
        return pending_id;
    case SendConfirmation::Outcome::Failed:
        return Error("request_failed", "sendMessage: " + confirm.error());
    case SendConfirmation::Outcome::Pending:
    default:
        return Error("request_failed",
                     "sendMessage: timed out waiting for the server to accept the message");
    }
}

} // namespace tgcurl

#endif // TGCURL_SEND_CONFIRM_H
