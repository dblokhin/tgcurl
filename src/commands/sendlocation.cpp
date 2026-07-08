// sendlocation "<id>" <latitude> <longitude> — send a static map point.
//
// inputMessageLocation with live_period 0 (a fixed point, not a live
// location). Coordinates are validated offline; everything after that is the
// shared send pipeline (send_common.h).
#include "error.h"
#include "send_common.h"

#include <optional>
#include <ostream>
#include <string>
#include <td/telegram/td_api.h>
#include <utility>
#include <variant>

namespace tgcurl {

namespace td_api = td::td_api;

namespace {

// A location is a text-sized payload; the text-send budget fits.
constexpr double kSendLocationTimeoutSeconds = 30.0;

constexpr const char* kUsage = R"(sendlocation "<chat_id|@username>" <latitude> <longitude> )"
                               R"([--reply-to <message_id>] [--silent] [--at <unix_time>])";

// Parse a coordinate: a finite double within [-bound, bound], whole lexeme
// consumed (rejects "12.3abc").
std::optional<double> parse_coordinate(const std::string& value, double bound) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size() || parsed < -bound || parsed > bound) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

namespace commands {

std::optional<Error> sendlocation(const Args& args, std::ostream& out) {
    if (args.size() < 3) {
        return Error("usage", kUsage);
    }
    const std::string& id_arg = args[0];
    const std::optional<double> latitude = parse_coordinate(args[1], 90.0);
    if (!latitude) {
        return Error("usage", "latitude must be a number in [-90, 90]");
    }
    const std::optional<double> longitude = parse_coordinate(args[2], 180.0);
    if (!longitude) {
        return Error("usage", "longitude must be a number in [-180, 180]");
    }
    std::variant<SendFlags, Error> flags = parse_send_flags(args, 3);
    if (std::holds_alternative<Error>(flags)) {
        return std::get<Error>(flags);
    }

    auto content = td_api::make_object<td_api::inputMessageLocation>();
    content->location_ =
        td_api::make_object<td_api::location>(*latitude, *longitude, /*horizontal_accuracy=*/0.0);
    content->live_period_ = 0; // a static point, not a live location

    return send_content(id_arg, std::move(content), std::get<SendFlags>(flags),
                        kSendLocationTimeoutSeconds, out);
}

} // namespace commands
} // namespace tgcurl
