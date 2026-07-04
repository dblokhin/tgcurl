// Shared setup for the head-less commands.
//
// Every non-login command needs the same preamble: load the config, spin up a
// TdClient, and drive it to an authorized state without prompting (so a missing
// session fails as not_authorized instead of hanging). open_session() bundles
// that. On success the caller owns a ready-to-use TdClient; on failure it gets
// the Error to return straight to main's dispatch.
#ifndef TGCURL_SESSION_H
#define TGCURL_SESSION_H

#include "error.h"
#include "tdclient.h"

#include <memory>
#include <variant>

namespace tgcurl {

// Load config + authenticate head-lessly. Returns a ready TdClient (owned by
// the caller) or an Error:
//   - config_missing / config_invalid  (from config load),
//   - not_authorized                    (no valid session; run: tgcurl login),
//   - the underlying TDLib/auth error otherwise.
std::variant<std::unique_ptr<TdClient>, Error> open_session();

} // namespace tgcurl

#endif // TGCURL_SESSION_H
