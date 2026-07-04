// Resolve a command-line identifier to a TDLib chat_id.
//
// tgcurl's addressing rule (see DESIGN.md → Peer identification):
//   - a purely numeric arg IS a chat_id, validated via getChat (a local lookup):
//     it must be "warm" — known to TDLib's cache from a prior fetch/run — or it
//     is rejected with guidance to run a list command first;
//   - an arg starting with '@' is a public username, resolved via
//     searchPublicChat (works for public users/channels/supergroups);
//   - anything else is unresolvable — no fuzzy name matching.
// classify() is the pure part (unit-testable, no network); resolve() adds the
// getChat / searchPublicChat round-trip for the id / username cases.
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

// Resolve an identifier to a chat_id, opening the chat as a side effect
// (getChat for a numeric id, searchPublicChat for a username) so it is known to
// this client before use. Returns the chat_id or an Error:
//   - "unresolvable" for a non-numeric, non-'@' arg, an unknown chat_id, or an
//     unknown public username,
//   - carrying the underlying TDLib error text.
std::variant<std::int64_t, Error> resolve_id(TdClient& client, const std::string& arg);

} // namespace tgcurl

#endif // TGCURL_RESOLVE_H
