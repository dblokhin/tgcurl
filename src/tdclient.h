// Synchronous wrapper over TDLib's asynchronous ClientManager.
//
// TDLib is event-driven: you send() a request with an id and later receive()
// responses and updates interleaved. tgcurl is one-shot per command, so this
// wrapper turns that loop into blocking calls: send_query() sends a request
// and pumps receive() until the matching response arrives, dispatching any
// updates that show up meanwhile to a registered handler (the auth state
// machine relies on this). This is the single place the rest of tgcurl talks
// to TDLib through.
#ifndef TGCURL_TDCLIENT_H
#define TGCURL_TDCLIENT_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

namespace tgcurl {

// Alias for a TDLib API object pointer (response or update payload).
using Object = td::td_api::object_ptr<td::td_api::Object>;

class TdClient {
  public:
    TdClient();
    ~TdClient();

    TdClient(const TdClient&) = delete;
    TdClient& operator=(const TdClient&) = delete;
    TdClient(TdClient&&) = delete;
    TdClient& operator=(TdClient&&) = delete;

    // Handler invoked for every incoming update (request_id == 0). Set this
    // before driving auth so updateAuthorizationState is observed. The handler
    // receives ownership of the update object.
    using UpdateHandler = std::function<void(td::td_api::object_ptr<td::td_api::Object>)>;
    void set_update_handler(UpdateHandler handler);

    // Send a request and block until its response arrives (up to
    // timeout_seconds of wall-clock), dispatching any updates seen while
    // waiting to the update handler. The returned object is the response and
    // may be a td_api::error — callers check with is_error(). On timeout it
    // returns a synthetic error (code kTimeoutErrorCode).
    Object send_query(td::td_api::object_ptr<td::td_api::Function> request,
                      double timeout_seconds = 30.0);

    // Pump receive() once (or until timeout), dispatching a single update to
    // the handler if one arrives. Returns true if an update was dispatched.
    //
    // NOTE: TDLib sends nothing — not even the first authorization state —
    // until the client has received its first request. So this cannot be used
    // to reach the *initial* auth state on its own: issue a send_query first
    // (e.g. getAuthorizationState) to activate the client, then use this to
    // drain any further updates.
    bool pump_updates(double timeout_seconds);

  private:
    Object await_response(std::uint64_t request_id, double timeout_seconds);

    std::unique_ptr<td::ClientManager> manager_;
    std::int32_t client_id_ = 0;
    std::uint64_t next_request_id_ = 1;
    UpdateHandler update_handler_;
};

// True if the object is a td_api::error.
[[nodiscard]] bool is_error(const Object& obj);

// If obj is a td_api::error, returns "<code>: <message>"; otherwise "".
[[nodiscard]] std::string error_text(const Object& obj);

} // namespace tgcurl

#endif // TGCURL_TDCLIENT_H
