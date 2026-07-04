// Resolve a command-line identifier to a TDLib chat_id.
//
// tgcurl's addressing rule (see DESIGN.md → Peer identification):
//   - a purely numeric arg IS a chat_id, used directly (fast path);
//   - an arg starting with '@' is a public username, resolved via
//     searchPublicChat (works for public users/channels/supergroups);
//   - anything else is unresolvable — no fuzzy name matching.
// classify() is the pure part (unit-testable, no network); resolve() adds the
// searchPublicChat round-trip for the username case.
#ifndef TGCURL_RESOLVE_H
#define TGCURL_RESOLVE_H

#include "error.h"
#include "tdclient.h"

#include <cstdint>
#include <string>
#include <variant>

namespace tgcurl {

// How an identifier argument is addressed.
enum class IdKind : std::uint8_t { ChatId, Username, Unresolvable };

// Pure classification of an identifier argument. For ChatId, chat_id is set;
// for Username, username holds the '@'-stripped name; for Unresolvable both are
// empty. No network, no TDLib.
struct Classified {
    IdKind kind = IdKind::Unresolvable;
    std::int64_t chat_id = 0;
    std::string username;
};

// Classify an identifier per the addressing rule above.
[[nodiscard]] Classified classify(const std::string& arg);

// Resolve an identifier to a chat_id, performing searchPublicChat for the
// username case. Returns the chat_id or an Error:
//   - "unresolvable" for a non-numeric, non-'@' arg,
//   - the underlying TDLib error if searchPublicChat fails.
std::variant<std::int64_t, Error> resolve_id(TdClient& client, const std::string& arg);

} // namespace tgcurl

#endif // TGCURL_RESOLVE_H
