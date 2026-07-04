// Minimal test scaffolding — no external framework, to keep deps lean.
// Each test file defines TESTS and calls RUN_TESTS() in main.
#ifndef TGCURL_TEST_UTIL_H
#define TGCURL_TEST_UTIL_H

#include <cstdio>
#include <string>

namespace tgcurl::test {
inline int g_failures = 0;

inline void check(bool cond, const char *expr, const char *file, int line) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
        ++g_failures;
    }
}

inline void check_eq(const std::string &got, const std::string &want, const char *file, int line) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s:%d:\n  got:  %s\n  want: %s\n", file, line, got.c_str(),
                     want.c_str());
        ++g_failures;
    }
}
} // namespace tgcurl::test

#define CHECK(cond) ::tgcurl::test::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(got, want) ::tgcurl::test::check_eq((got), (want), __FILE__, __LINE__)
#define RETURN_TEST_RESULT() return ::tgcurl::test::g_failures == 0 ? 0 : 1

#endif // TGCURL_TEST_UTIL_H
