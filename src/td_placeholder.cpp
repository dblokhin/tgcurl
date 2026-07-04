// Placeholder translation unit for the TDLib-backed layer (tgcurl_td).
//
// This target links Td::TdStatic PRIVATE and will hold tdclient (issue #3),
// auth, and the command bodies. Until tdclient lands we keep one trivial TU
// here that references TDLib, so the TD link seam is exercised by the build.
// Replace/remove when src/tdclient.cpp arrives.
#include <td/telegram/Client.h>

namespace tgcurl::td_internal {

// Returns TDLib's client-manager-backed API layer availability. Trivial: it
// just proves we can instantiate a ClientManager against the linked library.
bool tdlib_linked() {
    td::ClientManager manager;
    return manager.create_client_id() >= 0;
}

} // namespace tgcurl::td_internal
