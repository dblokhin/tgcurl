#include "auth.h"

#include <string>
#include <td/telegram/td_api.h>
#include <utility>

namespace tgcurl {

namespace td_api = td::td_api;

namespace {

// tgcurl identifies itself to Telegram with these static strings.
constexpr const char* kDeviceModel = "tgcurl";
constexpr const char* kSystemVersion = "cli";

// Build the setTdlibParameters request for a one-shot CLI: no message/chat
// cache, no secret chats, session persisted under config's database_directory.
//
// As of TDLib 1.8.63 setTdlibParameters is flat (the nested `tdlibParameters`
// object was removed) and carries `database_encryption_key` directly, so the
// former WaitEncryptionKey/checkDatabaseEncryptionKey step no longer exists.
// We pass an empty key: the local session DB is unencrypted at rest.
td_api::object_ptr<td_api::setTdlibParameters> make_parameters(const Config& config) {
    auto params = td_api::make_object<td_api::setTdlibParameters>();
    params->use_test_dc_ = false;
    params->database_directory_ = database_dir();
    params->files_directory_ = database_dir() + "/files";
    params->database_encryption_key_ = std::string();
    params->use_file_database_ = false;
    params->use_chat_info_database_ = false;
    params->use_message_database_ = false;
    params->use_secret_chats_ = false;
    params->api_id_ = config.api_id;
    params->api_hash_ = config.api_hash;
    params->system_language_code_ = "en";
    params->device_model_ = kDeviceModel;
    params->system_version_ = kSystemVersion;
    params->application_version_ = "0.1.0";
    return params;
}

// Turn a TDLib response that should be `ok` into an optional Error.
std::optional<Error> as_error(const Object& response, const std::string& what) {
    if (is_error(response)) {
        return Error("auth_failed", what + ": " + error_text(response));
    }
    return std::nullopt;
}

// Drives one authorization state. Returns:
//   - AuthResult with authorized=true when Ready is reached,
//   - AuthResult with an error to abort,
//   - std::nullopt to keep looping (state advanced, fetch the next one).
// `interacted` is set true the first time we actually prompt the user, so the
// caller can report already=true when nothing was needed.
class Flow {
  public:
    // Holds pointers (not references) so the class stays assignable and to
    // satisfy no-ref-member linting; all outlive the short-lived Flow.
    Flow(TdClient& client, const Config& config, Prompter& prompter)
        : client_(&client), config_(&config), prompter_(&prompter) {}

    AuthResult run() {
        // Activating the client and fetching the first state in one request:
        // getAuthorizationState both wakes TDLib (which sends nothing until the
        // first request) and returns the current state synchronously.
        //
        // The flow is bounded: each real state either advances to a distinct
        // next state or is terminal. As a guard against a pathological state
        // that neither advances nor terminates, cap the number of steps — far
        // more than the ~5 real transitions, so a legitimate login never hits
        // it, but a stuck server can't spin us forever.
        constexpr int kMaxSteps = 32;
        Object state = client_->send_query(td_api::make_object<td_api::getAuthorizationState>());
        for (int step = 0; step < kMaxSteps; ++step) {
            if (is_error(state)) {
                return fail(Error("auth_failed", "getAuthorizationState: " + error_text(state)));
            }
            std::optional<AuthResult> done = handle(state);
            if (done.has_value()) {
                return *done;
            }
            // State handled and advanced; fetch the next one.
            state = client_->send_query(td_api::make_object<td_api::getAuthorizationState>());
        }
        return fail(Error("auth_failed", "authorization did not converge"));
    }

  private:
    static AuthResult fail(Error err) { return AuthResult{false, false, std::move(err)}; }

    // Handle one authorization state. Returns a terminal AuthResult, or nullopt
    // to advance the loop.
    std::optional<AuthResult> handle(const Object& state) {
        switch (state->get_id()) {
        case td_api::authorizationStateWaitTdlibParameters::ID:
            return send_step(client_->send_query(make_parameters(*config_)), "setTdlibParameters");

        case td_api::authorizationStateWaitPhoneNumber::ID:
            return step_phone();

        case td_api::authorizationStateWaitCode::ID:
            return step_code();

        case td_api::authorizationStateWaitPassword::ID:
            return step_password(state);

        case td_api::authorizationStateReady::ID:
            return AuthResult{true, !interacted_, std::nullopt};

        case td_api::authorizationStateLoggingOut::ID:
        case td_api::authorizationStateClosing::ID:
        case td_api::authorizationStateClosed::ID:
            return fail(Error("not_authorized", "run: tgcurl login"));

        default:
            // authorizationStateWaitRegistration, WaitOtherDeviceConfirmation,
            // etc. — tgcurl doesn't support onboarding a brand-new account.
            return fail(Error("auth_failed", "unsupported authorization state"));
        }
    }

    // Send a request whose success is signalled by an `ok` response; on error
    // abort the flow, otherwise advance.
    static std::optional<AuthResult> send_step(const Object& response, const std::string& what) {
        if (std::optional<Error> err = as_error(response, what)) {
            return fail(*err);
        }
        return std::nullopt;
    }

    std::optional<AuthResult> step_phone() {
        if (!prompter_->interactive()) {
            return fail(Error("not_authorized", "run: tgcurl login"));
        }
        interacted_ = true;
        std::optional<std::string> phone = prompter_->prompt_phone();
        if (!phone.has_value() || phone->empty()) {
            return fail(Error("auth_failed", "no phone number provided"));
        }
        Object resp = client_->send_query(td_api::make_object<td_api::setAuthenticationPhoneNumber>(
            *phone, /*settings=*/nullptr));
        return send_step(resp, "setAuthenticationPhoneNumber");
    }

    std::optional<AuthResult> step_code() {
        if (!prompter_->interactive()) {
            return fail(Error("not_authorized", "run: tgcurl login"));
        }
        interacted_ = true;
        std::optional<std::string> code = prompter_->prompt_code();
        if (!code.has_value() || code->empty()) {
            return fail(Error("auth_failed", "no login code provided"));
        }
        Object resp =
            client_->send_query(td_api::make_object<td_api::checkAuthenticationCode>(*code));
        return send_step(resp, "checkAuthenticationCode");
    }

    std::optional<AuthResult> step_password(const Object& state) {
        if (!prompter_->interactive()) {
            return fail(Error("not_authorized", "run: tgcurl login"));
        }
        interacted_ = true;
        // Safe downcast: get_id() matched WaitPassword.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& wp = static_cast<const td_api::authorizationStateWaitPassword&>(*state);
        std::optional<std::string> pw = prompter_->prompt_password(wp.password_hint_);
        if (!pw.has_value() || pw->empty()) {
            return fail(Error("auth_failed", "no 2FA password provided"));
        }
        Object resp =
            client_->send_query(td_api::make_object<td_api::checkAuthenticationPassword>(*pw));
        return send_step(resp, "checkAuthenticationPassword");
    }

    TdClient* client_;
    const Config* config_;
    Prompter* prompter_;
    bool interacted_ = false;
};

} // namespace

AuthResult authenticate(TdClient& client, const Config& config, Prompter& prompter) {
    Flow flow(client, config, prompter);
    return flow.run();
}

} // namespace tgcurl
