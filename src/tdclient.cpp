#include "tdclient.h"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace tgcurl {

namespace td_api = td::td_api;

namespace {
// Synthetic error code for a tgcurl-side timeout (not a real TDLib code, which
// are positive HTTP-like codes). Distinctive so a caller switching on code_
// can tell it apart from a genuine TDLib error.
constexpr std::int32_t kTimeoutErrorCode = -1000;

// Upper bound on how long the destructor waits for a graceful close (databases
// flushed) before giving up so process exit can't hang. A local flush completes
// in well under this; the cap only guards a wedged/offline shutdown.
constexpr double kCloseTimeoutSeconds = 5.0;

// Quiet TDLib's logger. By default TDLib floods stderr with verbose logs, which
// would bury the interactive login prompts (also on stderr, since stdout is
// JSON-only). Level 0 = fatal-only. Setting TGCURL_DEBUG to any non-empty value
// restores a verbose trace (level 5) for debugging.
void configure_tdlib_logging() {
    std::int32_t level = 0;
    if (const char* dbg = std::getenv("TGCURL_DEBUG"); dbg != nullptr && *dbg != '\0') {
        level = 5;
    }
    // execute() is a static, synchronous call — usable before any client exists.
    td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(level));
}
} // namespace

TdClient::TdClient() : manager_(std::make_unique<td::ClientManager>()) {
    configure_tdlib_logging();
    // Can't use a member initializer: create_client_id() needs manager_ built.
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    client_id_ = manager_->create_client_id();
}

TdClient::~TdClient() {
    // TDLib flushes its databases to disk only on a graceful close: it persists
    // use_chat_info_database (the peer cache that keeps a chat_id usable across
    // runs) when it receives `close` and finishes shutting down. Just dropping
    // the ClientManager would exit before that flush, silently losing everything
    // written this run. So request close and pump events until TDLib reports
    // authorizationStateClosed — the documented "databases flushed, safe to
    // exit" signal (see tdlib td_example.cpp). Bounded by a timeout and fully
    // exception-safe: a destructor must never throw or hang the process.
    try {
        manager_->send(client_id_, next_request_id_++, td_api::make_object<td_api::close>());
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::duration<double>(kCloseTimeoutSeconds);
        while (std::chrono::steady_clock::now() < deadline) {
            const double remaining =
                std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
            auto response = manager_->receive(remaining);
            if (response.object == nullptr) {
                continue;
            }
            // The close-complete signal is an updateAuthorizationState update
            // (request_id == 0) carrying authorizationStateClosed.
            if (response.request_id == 0 &&
                response.object->get_id() == td_api::updateAuthorizationState::ID) {
                const td_api::Object* obj = response.object.get();
                // Safe downcast: get_id() matched updateAuthorizationState.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
                const auto* upd = static_cast<const td_api::updateAuthorizationState*>(obj);
                if (upd->authorization_state_ != nullptr &&
                    upd->authorization_state_->get_id() == td_api::authorizationStateClosed::ID) {
                    break;
                }
            }
        }
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
        // Best-effort flush; never let shutdown throw out of a destructor.
    }
}

void TdClient::set_update_handler(UpdateHandler handler) {
    update_handler_ = std::move(handler);
}

Object TdClient::send_query(td_api::object_ptr<td_api::Function> request, double timeout_seconds) {
    const std::uint64_t request_id = next_request_id_++;
    manager_->send(client_id_, request_id, std::move(request));
    return await_response(request_id, timeout_seconds);
}

// request_id (an opaque id) and timeout_seconds (a duration) are unlike
// concepts with a single call site; a transposition here isn't a real risk.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Object TdClient::await_response(std::uint64_t request_id, double timeout_seconds) {
    // Block until the response for request_id arrives, dispatching any updates
    // (request_id == 0) seen while waiting. timeout_seconds is a wall-clock
    // deadline: on an active account a steady stream of updates would otherwise
    // reset a per-receive() timeout indefinitely, so we shrink the receive()
    // budget each cycle and bail with a synthetic error once it's exhausted.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_seconds);
    while (true) {
        const double remaining =
            std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0.0) {
            return td_api::make_object<td_api::error>(kTimeoutErrorCode,
                                                      "timeout waiting for response");
        }
        auto response = manager_->receive(remaining);
        if (response.object == nullptr) {
            // No event this cycle; loop to re-check the deadline.
            continue;
        }
        if (response.request_id == request_id) {
            return std::move(response.object);
        }
        if (response.request_id == 0) {
            // An update; note client-level state, then hand it to the
            // observer if one is registered.
            observe_update(response.object);
            if (update_handler_) {
                update_handler_(std::move(response.object));
            }
            continue;
        }
        // A response to a different in-flight request; tgcurl is one-shot and
        // sends serially, so this shouldn't happen — drop it and keep waiting.
    }
}

bool TdClient::pump_updates(double timeout_seconds) {
    auto response = manager_->receive(timeout_seconds);
    if (response.object == nullptr) {
        return false;
    }
    if (response.request_id == 0) {
        observe_update(response.object);
        if (update_handler_) {
            update_handler_(std::move(response.object));
        }
        return true;
    }
    // A stray response with no waiter; nothing to do with it here.
    return false;
}

void TdClient::observe_update(const Object& update) {
    if (std::optional<bool> ready = connection_ready_from_update(update); ready.has_value()) {
        connection_ready_ = *ready;
    }
}

bool TdClient::wait_connection_ready(double timeout_seconds) {
    // Same wall-clock deadline discipline as await_response: a steady stream
    // of unrelated updates must not extend the wait indefinitely.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_seconds);
    while (!connection_ready_) {
        const double remaining =
            std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0.0) {
            return false;
        }
        pump_updates(remaining);
    }
    return true;
}

std::optional<bool> connection_ready_from_update(const Object& update) {
    if (update == nullptr || update->get_id() != td_api::updateConnectionState::ID) {
        return std::nullopt;
    }
    // Safe downcast: get_id() already confirmed the dynamic type.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& upd = static_cast<const td_api::updateConnectionState&>(*update);
    return upd.state_ != nullptr && upd.state_->get_id() == td_api::connectionStateReady::ID;
}

bool is_error(const Object& obj) {
    return obj != nullptr && obj->get_id() == td_api::error::ID;
}

std::string error_text(const Object& obj) {
    if (!is_error(obj)) {
        return "";
    }
    // Safe downcast: is_error() already confirmed the dynamic type via get_id().
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& err = static_cast<const td_api::error&>(*obj);
    return std::to_string(err.code_) + ": " + err.message_;
}

std::string primary_username(const td_api::object_ptr<td_api::usernames>& usernames) {
    if (usernames == nullptr) {
        return {};
    }
    // active_usernames_[0] is the handle to show as primary; editable_username_
    // is only what the logged-in account may edit (empty for others).
    if (!usernames->active_usernames_.empty()) {
        return usernames->active_usernames_.front();
    }
    return usernames->editable_username_;
}

} // namespace tgcurl
