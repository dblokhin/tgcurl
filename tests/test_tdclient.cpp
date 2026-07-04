// Tests for the parts of tdclient that don't need a live TDLib server:
// error detection/formatting over td_api objects. The send_query round-trip
// is network-dependent and is exercised manually (see issue #9).
#include "tdclient.h"
#include "test_util.h"

#include <td/telegram/td_api.h>

using namespace tgcurl;
namespace td_api = td::td_api;

int main() {
    // is_error / error_text on an actual td_api::error object.
    {
        Object err = td_api::make_object<td_api::error>(400, "BAD_REQUEST");
        CHECK(is_error(err));
        CHECK_EQ(error_text(err), "400: BAD_REQUEST");
    }
    // A non-error object is not flagged, and yields empty text.
    {
        Object ok = td_api::make_object<td_api::ok>();
        CHECK(!is_error(ok));
        CHECK_EQ(error_text(ok), "");
    }
    // A null object is not an error (defensive).
    {
        Object none;
        CHECK(!is_error(none));
        CHECK_EQ(error_text(none), "");
    }

    // A client can be constructed and destroyed without touching the network.
    { TdClient client; }

    RETURN_TEST_RESULT();
}
