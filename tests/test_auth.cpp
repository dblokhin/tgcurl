// Unit tests for the auth pieces that don't need a live Telegram connection.
// The full authorization round-trip is network-dependent and is exercised
// manually (see issue #9). Here we pin down the head-less prompter's contract,
// which is what makes non-login commands fail fast instead of hanging.
#include "auth.h"
#include "test_util.h"

using namespace tgcurl;

int main() {
    HeadlessPrompter prompter;

    // A head-less prompter must never claim to be interactive — that's the
    // signal authenticate() uses to refuse to prompt and return not_authorized.
    CHECK(!prompter.interactive());

    // Its prompt methods are defensive no-ops (never actually called by the
    // flow, but must not block or return a value if they somehow are).
    CHECK(!prompter.prompt_phone().has_value());
    CHECK(!prompter.prompt_code().has_value());
    CHECK(!prompter.prompt_password("some hint").has_value());

    RETURN_TEST_RESULT();
}
