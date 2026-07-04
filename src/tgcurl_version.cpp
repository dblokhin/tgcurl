#include "tgcurl_version.h"

#ifndef TGCURL_VERSION
#define TGCURL_VERSION "0.0.0-unknown"
#endif

namespace tgcurl {
const char* version() {
    return TGCURL_VERSION;
}
} // namespace tgcurl
