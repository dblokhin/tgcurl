// Build-time version string for tgcurl (set via TGCURL_VERSION).
#ifndef TGCURL_VERSION_H
#define TGCURL_VERSION_H

namespace tgcurl {
// Returns the compiled-in version string (from -DTGCURL_VERSION).
const char* version();
} // namespace tgcurl

#endif // TGCURL_VERSION_H
