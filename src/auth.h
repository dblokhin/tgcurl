// TDLib authorization: drive updateAuthorizationState to a logged-in session.
//
// TDLib gates every real request behind an authorization state machine
// (WaitTdlibParameters → WaitPhoneNumber → WaitCode → [WaitPassword] → Ready).
// This module runs that machine to completion. The session persists in the
// config's database_directory, so on a subsequent run TDLib jumps straight to
// Ready and authenticate() returns without asking for anything — that's how
// "log in once" works.
//
// The one difference between `login` and the head-less commands is *who
// answers the prompts*: login supplies an interactive TTY prompter; every
// other command supplies a head-less prompter that refuses to prompt, so a
// missing session surfaces as a not_authorized error instead of hanging on
// stdin. Both share this exact same flow.
#ifndef TGCURL_AUTH_H
#define TGCURL_AUTH_H

#include "config.h"
#include "error.h"
#include "tdclient.h"

#include <optional>
#include <string>

namespace tgcurl {

// Supplies the interactive answers the auth flow may need. login provides a
// TTY-backed implementation; head-less commands provide HeadlessPrompter,
// whose methods signal "can't prompt" so the flow aborts with not_authorized.
class Prompter {
  public:
    Prompter() = default;
    Prompter(const Prompter&) = default;
    Prompter& operator=(const Prompter&) = default;
    Prompter(Prompter&&) = default;
    Prompter& operator=(Prompter&&) = default;
    virtual ~Prompter() = default;

    // Whether this prompter may interact with the user at all. When false, the
    // flow never calls the prompt_* methods and instead fails not_authorized as
    // soon as a state needs input.
    [[nodiscard]] virtual bool interactive() const = 0;

    // Prompt for the respective credential. Called only when interactive().
    // Returning std::nullopt means the user gave up (EOF/empty) — treated as a
    // login failure.
    virtual std::optional<std::string> prompt_phone() = 0;
    virtual std::optional<std::string> prompt_code() = 0;
    virtual std::optional<std::string> prompt_password(const std::string& hint) = 0;
};

// A prompter for head-less commands: never interactive; its prompt_* methods
// are never invoked (they return nullopt defensively).
class HeadlessPrompter final : public Prompter {
  public:
    [[nodiscard]] bool interactive() const override { return false; }
    std::optional<std::string> prompt_phone() override { return std::nullopt; }
    std::optional<std::string> prompt_code() override { return std::nullopt; }
    std::optional<std::string> prompt_password(const std::string& /*hint*/) override {
        return std::nullopt;
    }
};

// Outcome of running the authorization flow.
struct AuthResult {
    bool authorized = false;    // reached authorizationStateReady
    bool already = false;       // reached Ready with no interaction (session reuse)
    std::optional<Error> error; // set iff authorized == false
};

// Drive the authorization state machine on `client` using `config` for the
// TDLib parameters and `prompter` for any interactive input. Sets up the
// client's update handler internally. On success returns authorized == true
// (already == true if no prompt was needed). On failure returns an Error:
//   - "not_authorized" when a head-less run needs input it can't get,
//   - "auth_failed" when an interactive step is abandoned or TDLib rejects it,
//   - the underlying TDLib error otherwise.
AuthResult authenticate(TdClient& client, const Config& config, Prompter& prompter);

} // namespace tgcurl

#endif // TGCURL_AUTH_H
