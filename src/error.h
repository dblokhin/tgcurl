// Structured errors for tgcurl.
//
// Every failure surfaces as a JSON object on stderr plus a non-zero exit code,
// so an AI agent or script can detect and parse it. An Error carries a stable
// machine code and a human-oriented hint.
#ifndef TGCURL_ERROR_H
#define TGCURL_ERROR_H

#include "json_out.h"

#include <ostream>
#include <string>

namespace tgcurl {

class Error {
  public:
    Error(std::string code, std::string hint) : code_(std::move(code)), hint_(std::move(hint)) {}

    [[nodiscard]] const std::string& code() const { return code_; }
    [[nodiscard]] const std::string& hint() const { return hint_; }

    // {"error":"<code>","hint":"<hint>"}
    [[nodiscard]] std::string to_json() const {
        json::Writer w;
        w.field("error", code_);
        if (!hint_.empty()) {
            w.field("hint", hint_);
        }
        return w.object();
    }

  private:
    std::string code_;
    std::string hint_;
};

// Print an error as JSON to the given stream (stderr in production).
inline void emit_error(const Error& e, std::ostream& os) {
    os << e.to_json() << '\n';
}

// Exit code convention: 1 for any handled error.
constexpr int kExitError = 1;

} // namespace tgcurl

#endif // TGCURL_ERROR_H
