#include "session.h"

#include "auth.h"
#include "config.h"

#include <memory>
#include <variant>

namespace tgcurl {

std::variant<std::unique_ptr<TdClient>, Error> open_session() {
    std::variant<Config, Error> cfg = load_config();
    if (std::holds_alternative<Error>(cfg)) {
        return std::get<Error>(cfg);
    }
    const Config& config = std::get<Config>(cfg);

    auto client = std::make_unique<TdClient>();
    HeadlessPrompter prompter;
    AuthResult result = authenticate(*client, config, prompter);
    if (!result.authorized) {
        if (result.error.has_value()) {
            return *result.error;
        }
        return Error("not_authorized", "run: tgcurl login");
    }
    return client;
}

} // namespace tgcurl
