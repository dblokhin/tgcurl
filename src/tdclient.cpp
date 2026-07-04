#include "tdclient.h"

#include <chrono>
#include <string>
#include <utility>

namespace tgcurl {

namespace td_api = td::td_api;

namespace {
// Synthetic error code for a tgcurl-side timeout (not a real TDLib code, which
// are positive HTTP-like codes). Distinctive so a caller switching on code_
// can tell it apart from a genuine TDLib error.
constexpr std::int32_t kTimeoutErrorCode = -1000;
} // namespace

TdClient::TdClient() : manager_(std::make_unique<td::ClientManager>()) {
    // Can't use a member initializer: create_client_id() needs manager_ built.
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    client_id_ = manager_->create_client_id();
}

TdClient::~TdClient() = default;

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
            // An update; hand it to the observer if one is registered.
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
        if (update_handler_) {
            update_handler_(std::move(response.object));
        }
        return true;
    }
    // A stray response with no waiter; nothing to do with it here.
    return false;
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

} // namespace tgcurl
