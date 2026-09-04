// Sunbright invariant assertions.
//
// SB_ASSERT(cond, fmt, ...) — assert an invariant that MUST hold ("should not happen / has to
// happen"). Unlike <cassert>'s assert(), this is **always active**, including Release builds:
// the runtime's whole discipline is to fail FAST the instant guest state is corrupt
// (a wild branch, a bad stack pointer, an impossible scheduler state) rather than limp on and
// scribble over memory. On failure it prints the message + a native backtrace, suppresses the
// (multi-GB binary) core dump so the process exits fast instead of hanging in systemd-coredump
// (see the abort-coredump-hang lesson), and abort()s.
//
// SB_FATAL(fmt, ...) — unconditional failure at a point that must be unreachable.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <sb_log.h>
#include <sys/resource.h>

[[noreturn]] inline void sb_assert_fail(const char* expr, const char* file, int line,
                                        const char* msg) {
    sb_errorf("assert", "ASSERT FAILED: %s at %s:%d%s%s", expr, file, line, msg && *msg ? ": " : "",
              msg && *msg ? msg : "");
    void* bt[96];
    int bn = backtrace(bt, 96);
    backtrace_symbols_fd(bt, bn, fileno(stderr));
    std::fflush(stderr);
    struct rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core); // skip the multi-GB core so we exit fast, not hang
    std::abort();
}

// Format helper so call sites can pass printf-style context without each repeating the plumbing.
#define SB_FAIL_FMT(...)                                                                           \
    ([&]() -> const char* {                                                                        \
        static thread_local char _sb_buf[512];                                                     \
        std::snprintf(_sb_buf, sizeof(_sb_buf), __VA_ARGS__);                                      \
        return _sb_buf;                                                                            \
    }())

#define SB_ASSERT(cond, ...)                                                                       \
    do {                                                                                           \
        if (!(cond))                                                                               \
            sb_assert_fail(#cond, __FILE__, __LINE__, SB_FAIL_FMT(__VA_ARGS__));                   \
    } while (0)

#define SB_FATAL(...) sb_assert_fail("SB_FATAL", __FILE__, __LINE__, SB_FAIL_FMT(__VA_ARGS__))
